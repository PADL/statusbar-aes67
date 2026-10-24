#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An AES67 sender: a FrameProvider's frames by PTP sample index, sent as RTP on the sample-index
/// grid with `mediaclk:direct=0`, and the stream's SDP. configure(), clear(), set_grandmaster(),
/// set_name() and session() allocate; tick() and the accessors are real-time safe.

#include "statusbar/aes67/aes67_stream.hpp"
#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/sdp/sdp.hpp"
#include "statusbar/status/status.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace statusbar::aes67 {

/// A grandmaster written `<EUI-64>[:<domain>]` (the EUI-64 with '-' or ':'
/// separators, or none; the domain 0-127), as a command line or configuration
/// gives it; nullopt if it is not one.
[[nodiscard]] auto parse_grandmaster(std::string_view text) -> std::optional<Grandmaster>;

class Sender
{
  public:
    /// Packets one tick sends at most; further behind, the sender skips ahead.
    static constexpr uint32_t MAX_BURST = 8;
    /// The largest payload sent, 1460 octets: a 1500-octet MTU less the 20-octet IPv4, 8-octet UDP
    /// and 12-octet RTP headers.
    static constexpr uint32_t MAX_PAYLOAD = 1500 - 20 - 8 - 12;
    /// Session ids and versions are kept to 63 bits: positive in any signed reading.
    static constexpr uint64_t SESSION_ID_MASK = 0x7FFF'FFFF'FFFF'FFFFULL;

    /// `name` is the session name (s=) of session().
    Sender(StreamFormat const& format, FrameProvider provider, Transport& transport, std::string name);

    /// Send to `destination`, replacing any before.
    [[nodiscard]] auto configure(Destination const& destination) -> Status;
    /// Send to `session.media[0]`'s connection, port and the payload type of its
    /// offered format with the sender's encoding; channels and rate must be the sender's.
    [[nodiscard]] auto configure(sdp::Session const& session) -> Status;
    /// Stop sending.
    void clear() noexcept;

    /// The read delay (link offset) in frames: a packet is sent once its last frame is this far
    /// behind now. Default: default_read_delay(). Any thread, real-time safe; the next tick takes it.
    void set_read_delay(uint32_t const frames) noexcept { read_delay_.store(frames, std::memory_order_relaxed); }
    [[nodiscard]] auto read_delay() const noexcept -> uint32_t { return read_delay_.load(std::memory_order_relaxed); }
    /// One 125 us tick, rounded up (6 frames at 48 kHz): for a provider whose frames are there by
    /// their index; a provider fed from the network wants relay_read_delay().
    [[nodiscard]] static constexpr auto default_read_delay(StreamFormat const& format) noexcept -> uint32_t
    {
        return (format.sample_rate + 7999) / 8000;
    }
    /// One packet of this stream plus two 125 us ticks: for a provider whose
    /// frames arrive over a network (an AES67 receiver's), a packet time plus
    /// arrival jitter after their index.
    [[nodiscard]] static constexpr auto relay_read_delay(StreamFormat const& format) noexcept -> uint32_t
    {
        return format.frames_per_packet + (format.sample_rate / 4000);
    }

    /// Where the packet grid starts, in frames (taken modulo frames_per_packet):
    /// packets carry [k * fpp + phase, (k + 1) * fpp + phase). Call before
    /// configure(), off the real-time thread.
    void set_packet_phase(uint32_t const frames) noexcept
    {
        phase_ = format_.frames_per_packet != 0 ? frames % format_.frames_per_packet : 0;
    }
    [[nodiscard]] auto packet_phase() const noexcept -> uint32_t { return phase_; }
    /// The phase that staggers the `k`th sender: (k mod 8) 125 us ticks modulo
    /// the packet (0, 6, ... 42 frames for 1 ms at 48 kHz).
    [[nodiscard]] static constexpr auto stagger_phase(StreamFormat const& format, uint32_t const k) noexcept -> uint32_t
    {
        if (format.frames_per_packet == 0) {
            return 0;
        }
        uint32_t const tick = (format.sample_rate + 7999) / 8000;
        return ((k % 8) * tick) % format.frames_per_packet;
    }

    /// The SDP session id (o=), stable for the same stream across restarts (RFC 8866 §5.2). 0, the
    /// default: the first configure() takes stable_session_id() of the origin address and the name.
    /// Call before configure(), off the real-time thread.
    void set_session_id(uint64_t const id) noexcept { session_id_ = id & SESSION_ID_MASK; }
    [[nodiscard]] auto session_id() const noexcept -> uint64_t { return session_id_; }
    /// A session id from `key` (FNV-1a), the same for the same key, positive
    /// in any signed 64-bit reading and never 0.
    [[nodiscard]] static constexpr auto stable_session_id(std::string_view const key) noexcept -> uint64_t
    {
        uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
        for (char const c : key) {
            hash = (hash ^ static_cast<uint8_t>(c)) * 0x0000'0100'0000'01B3ULL;
        }
        hash &= SESSION_ID_MASK;
        return hash == 0 ? 1 : hash;
    }

    /// The PTP grandmaster ts-refclk names; nullopt until it is known, and the
    /// SDP then has no ts-refclk. A change bumps the session version (o=), as
    /// configure() does.
    void set_grandmaster(std::optional<Grandmaster> const& grandmaster);

    [[nodiscard]] auto grandmaster() const noexcept -> std::optional<Grandmaster> const& { return grandmaster_; }

    /// The session name (s=) of session(), the constructor's until changed; a new name bumps the
    /// session version. Off the real-time thread.
    void set_name(std::string name);
    [[nodiscard]] auto name() const noexcept -> std::string const& { return name_; }

    /// Take `previous`'s read delay, grandmaster, name and SDP session id and version, so this
    /// sender's next configure() announces the same session one version on. Call before
    /// configure(), off the real-time thread.
    void continue_from(Sender const& previous);

    /// Send every packet due by `now_index` (the media clock's index at the
    /// current PTP time).
    void tick(uint64_t now_index) noexcept;

    /// The stream's SDP, or nullopt when not configured.
    [[nodiscard]] auto session() const -> std::optional<sdp::Session>;

    [[nodiscard]] auto format() const noexcept -> StreamFormat const& { return format_; }
    [[nodiscard]] auto state() const noexcept -> State { return state_; }
    [[nodiscard]] auto counters() const noexcept -> Counters const& { return counters_; }
    /// Why the transport refused the last configure(), or why it cannot send
    /// (Transport::send_error(), seen by the last tick), for State::error.
    [[nodiscard]] auto error() const noexcept -> std::error_code { return error_; }
    [[nodiscard]] auto configured() const noexcept -> bool { return port_ != nullptr; }
    [[nodiscard]] auto ssrc() const noexcept -> uint32_t { return ssrc_; }

  private:
    StreamFormat format_;
    FrameProvider provider_;
    Transport& transport_;
    std::string name_;
    /// One version on (the first: the wall clock's seconds, one on).
    void bump_session_version();
    /// Fill `payload` with the provider's frames from `first_index`, or
    /// silence when it has none; what the provider said.
    [[nodiscard]] auto render_payload(uint64_t first_index, std::span<uint8_t> payload) noexcept -> Supply;

    std::unique_ptr<SendPort> port_;
    Destination destination_;
    std::vector<uint8_t> payload_;  ///< staging, only for a packet the transport had no TX buffer for

    uint32_t ssrc_{0};
    uint16_t sequence_{0};
    uint64_t session_id_{0};
    uint64_t session_version_{0};
    std::optional<Grandmaster> grandmaster_;
    std::atomic<uint32_t> read_delay_;
    uint32_t phase_{0};
    bool started_{false};
    uint64_t next_index_{0};  ///< first frame of the next packet to send
    Counters counters_;
    State state_{State::idle};
    std::error_code error_;
};

}  // namespace statusbar::aes67
