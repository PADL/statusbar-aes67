#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// When an AES67 sender's packets go: the packet grid on the sample index, a packet due once its last frame is the send
/// offset behind now, resynchronisation, a run held for its first report, and each run's SSRC and sequence (RFC 3550
/// Section 8). seed() draws from the system; the rest is real-time safe, one thread.

#include "padl/aes67/aes67_plan.hpp"
#include "padl/aes67/aes67_stream.hpp"
#include "padl/mediaclock/mediaclock_ptp_clock.hpp"
#include "padl/rtp/rtp_header.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <random>
#include <utility>

namespace padl::aes67 {

class PacketSchedule
{
  public:
    /// Packets one tick sends at most; further behind, the schedule skips ahead.
    static constexpr uint32_t MAX_BURST = 8;
    /// How far ahead a held run's first packet is placed, in microseconds: time for the reactor to send its SR.
    static constexpr uint32_t FIRST_REPORT_LEAD_US = 20'000;

    explicit PacketSchedule(StreamFormat const& format) noexcept
        : format_{format}
    {}

    /// Where the grid starts, in frames (modulo frames_per_packet): packets carry [k * fpp + phase, (k + 1) * fpp + phase).
    void set_phase(uint32_t const frames) noexcept
    {
        phase_ = format_.frames_per_packet != 0 ? frames % format_.frames_per_packet : 0;
    }
    [[nodiscard]] auto phase() const noexcept -> uint32_t { return phase_; }

    /// A new generator seed from the system, a random SSRC (neither 0 nor the last) and sequence. Not real-time safe.
    void seed()
    {
        std::random_device random;
        rng_.seed((uint64_t{random()} << 32U) | random());
        ssrc_.store(fresh_ssrc(), std::memory_order_relaxed);  // never 0, nor the last run's
        sequence_ = static_cast<uint16_t>(random());
    }
    /// A new run's SSRC and sequence from the generator, nothing held.
    void renew() noexcept
    {
        ssrc_.store(fresh_ssrc(), std::memory_order_relaxed);
        sequence_ = static_cast<uint16_t>(next_random());
        held_first_.reset();
        placed_ = false;
    }
    /// Every run's first packet at least `frames` past the newest due packet, held until then as for a report; 0 (the
    /// default): a run starts at the newest due packet unless held.
    void set_start_lead(uint32_t const frames) noexcept { start_lead_ = frames; }
    [[nodiscard]] auto start_lead() const noexcept -> uint32_t { return start_lead_; }
    /// Starts `run`, its first packet due now (is_due()): what a frame port's engine does with the run it is given.
    void start(SendRun const& run) noexcept
    {
        ssrc_.store(run.ssrc, std::memory_order_relaxed);
        sequence_ = run.first_sequence;
        next_index_ = run_first_ = run.first_index;
        held_first_.reset();
        started_ = true;
    }
    /// Whether the packet at `first_index` is due at `now_index` with `send_offset`: its last frame that far behind.
    [[nodiscard]] auto is_due(uint64_t const first_index, uint64_t const now_index, uint64_t const send_offset) const noexcept
        -> bool
    {
        return first_index + format_.frames_per_packet + send_offset <= now_index + 1;
    }
    /// Not started: the next newest_due() starts a run on the grid.
    void stop() noexcept { started_ = false; }
    /// Not started, nothing held.
    void reset() noexcept
    {
        started_ = false;
        held_first_.reset();
        placed_ = false;
    }

    /// The newest packet due at `now_index` with `send_offset` (frames), next_index() resynchronised; a run's first
    /// packet waits for its report while `hold` (FIRST_REPORT_LEAD_US), or for the start lead (clear_placed()). Frames
    /// skipped, too far behind to send in time, are added to `lost`; nullopt when no packet is to be sent.
    [[nodiscard]] auto newest_due(uint64_t const now_index, uint64_t const send_offset, bool const hold, uint64_t& lost) noexcept
        -> std::optional<uint64_t>
    {
        uint64_t const fpp = format_.frames_per_packet;
        uint64_t const phase = phase_;
        if (now_index + 1 < send_offset + fpp + phase) {
            return std::nullopt;  // no packet due yet (only near the epoch)
        }
        // The newest packet (on the grid shifted by the phase) whose last frame is send_offset or more behind now.
        uint64_t const newest = ((((now_index + 1 - send_offset - phase) / fpp) - 1) * fpp) + phase;
        return resync(newest, hold, lost) ? std::optional{newest} : std::nullopt;
    }
    /// Whether newest_due() placed a held run's first packet (afresh) at held() since the last call.
    [[nodiscard]] auto clear_placed() noexcept -> bool { return std::exchange(placed_, false); }

    /// The next packet's RTP header: its sequence number and timestamp, in run `ssrc`.
    [[nodiscard]] auto header(uint8_t const payload_type, uint32_t const ssrc) const noexcept -> rtp::RtpHeader
    {
        rtp::RtpHeader header{};
        header.init(payload_type, sequence_, mediaclock::PtpMediaClock::rtp_timestamp(next_index_, 0), ssrc);
        return header;
    }
    /// On to the next packet.
    void advance() noexcept
    {
        ++sequence_;
        next_index_ += format_.frames_per_packet;
    }

    /// Back to the packet at `first_index`, behind next_index(), its sequence with it, so that packets that went nowhere
    /// go again: no further than MAX_BURST - 1 packets behind `newest`, on the grid, the frames skipped added to `lost`.
    /// The packets moved back; 0 when not started or `first_index` is not behind.
    [[nodiscard]] auto rewind(uint64_t const first_index, uint64_t const newest, uint64_t& lost) noexcept -> uint32_t
    {
        if (!started_ || format_.frames_per_packet == 0 || first_index >= next_index_) {
            return 0;
        }
        uint64_t const reach = uint64_t{MAX_BURST - 1} * format_.frames_per_packet;
        uint32_t const packets = packets_behind(std::max(first_index, newest > reach ? newest - reach : 0));
        lost += next_index_ - first_index - (uint64_t{packets} * format_.frames_per_packet);  // further back, or off the grid
        step_back(packets);
        return packets;
    }

    /// The current run's SSRC. Any thread.
    [[nodiscard]] auto ssrc() const noexcept -> uint32_t { return ssrc_.load(std::memory_order_relaxed); }
    [[nodiscard]] auto sequence() const noexcept -> uint16_t { return sequence_; }
    /// The first frame of the next packet to send.
    [[nodiscard]] auto next_index() const noexcept -> uint64_t { return next_index_; }
    /// The run's first packet.
    [[nodiscard]] auto run_first() const noexcept -> uint64_t { return run_first_; }
    [[nodiscard]] auto started() const noexcept -> bool { return started_; }
    /// The packet a held run starts at, once placed.
    [[nodiscard]] auto held() const noexcept -> std::optional<uint64_t> const& { return held_first_; }

  private:
    /// Moves next_index_ toward `newest` after a start or a clock step, frames skipped added to `lost`; false to wait
    /// out a small step back, or while a held run's first packet is not yet due (start_held()).
    [[nodiscard]] auto resync(uint64_t const newest, bool const hold, uint64_t& lost) noexcept -> bool
    {
        uint64_t const fpp = format_.frames_per_packet;
        if (!started_ && (hold || start_lead_ != 0) && !start_held(newest, hold)) {
            return false;  // the run's first packet waits for its SR, or for its start to come into force
        }
        if (!started_) {
            next_index_ = run_first_ = newest;
            started_ = true;
        } else if (next_index_ > newest + fpp) {
            // The clock went back. A small step is waited out (nothing is due
            // until now catches up, so no index is sent twice); a large one,
            // more than MAX_BURST packets, resynchronises.
            if (next_index_ - (newest + fpp) <= uint64_t{MAX_BURST} * fpp) {
                return false;
            }
            next_index_ = newest;
        } else if (next_index_ + (uint64_t{MAX_BURST} * fpp) <= newest) {
            lost += newest - next_index_;  // too far behind: skip what cannot be sent in time
            next_index_ = newest;
        }
        return true;
    }

    /// Whether a held run starts at `newest`: its first packet, held_first() ahead, is due; placed afresh (placed_)
    /// when none is yet, or the clock stepped back past it or forward past a burst (its SR would name a packet never
    /// sent).
    [[nodiscard]] auto start_held(uint64_t const newest, bool const hold) noexcept -> bool
    {
        uint64_t const burst = uint64_t{MAX_BURST - 1} * format_.frames_per_packet;
        if (!held_first_ || newest > *held_first_ + burst || *held_first_ > held_first(newest, hold)) {
            held_first_ = held_first(newest, hold);
            placed_ = true;
            return false;
        }
        if (newest < *held_first_) {
            return false;  // its SR has until then; the run starts then whether or not it went
        }
        next_index_ = run_first_ = *held_first_;
        started_ = true;
        return true;
    }

    /// The packet a held run starts at: FIRST_REPORT_LEAD_US past `newest` while `hold`, and at least the start lead,
    /// on the packet grid.
    [[nodiscard]] auto held_first(uint64_t const newest, bool const hold) const noexcept -> uint64_t
    {
        uint64_t const fpp = format_.frames_per_packet;
        uint64_t const report = hold ? (uint64_t{format_.sample_rate} * FIRST_REPORT_LEAD_US) / 1'000'000 : 0;
        uint64_t const lead = std::max<uint64_t>(report, start_lead_) + fpp - 1;
        return newest + ((lead / fpp) * fpp);
    }

    /// The whole packets from `first_index` up to next_index(); 0 when it is not behind.
    [[nodiscard]] auto packets_behind(uint64_t const first_index) const noexcept -> uint32_t
    {
        return first_index < next_index_ ? static_cast<uint32_t>((next_index_ - first_index) / format_.frames_per_packet) : 0U;
    }
    /// Back `packets` packets, the sequence with them.
    void step_back(uint32_t const packets) noexcept
    {
        next_index_ -= uint64_t{packets} * format_.frames_per_packet;
        sequence_ = static_cast<uint16_t>(sequence_ - packets);
    }

    /// Draws from rng_ (seeded by seed()): no syscall on the ticking thread.
    [[nodiscard]] auto next_random() noexcept -> uint32_t { return static_cast<uint32_t>(rng_() >> 32U); }
    /// A random SSRC, neither 0 nor the current one.
    [[nodiscard]] auto fresh_ssrc() noexcept -> uint32_t
    {
        uint32_t const old = ssrc();
        uint32_t next = next_random();
        while (next == 0 || next == old) {
            next = next_random();
        }
        return next;
    }

    StreamFormat format_;
    std::atomic<uint32_t> ssrc_{0};
    std::mt19937_64 rng_;
    uint16_t sequence_{0};
    uint32_t phase_{0};
    uint32_t start_lead_{0};
    bool started_{false};
    bool placed_{false};                  ///< start_held() placed a held run's first packet since clear_placed()
    uint64_t next_index_{0};              ///< first frame of the next packet to send
    uint64_t run_first_{0};               ///< the run's first packet
    std::optional<uint64_t> held_first_;  ///< the packet a run held for its first SR starts at
};

}  // namespace padl::aes67
