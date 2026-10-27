#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RTP sequence number validation (RFC 3550 Appendix A.1) with the loss accounting of Appendix
/// A.3: a new source on probation, gaps as losses, a large jump believed only when confirmed.

#include <algorithm>
#include <cstdint>

namespace statusbar::rtp {

class RtpSequence
{
  public:
    static constexpr uint32_t MIN_SEQUENTIAL = 2;
    static constexpr uint16_t MAX_DROPOUT = 3000;
    static constexpr uint16_t MAX_MISORDER = 100;

    /// Account for a packet; the first call starts tracking the source. False if
    /// the packet should be discarded: the source is still on probation (so the
    /// first packet always is), or the packet is an unconfirmed jump.
    constexpr auto update(uint16_t const seq) noexcept -> bool
    {
        if (!started_) {
            started_ = true;
            restart(seq);
            max_seq_ = static_cast<uint16_t>(seq - 1);
        }
        auto const delta = static_cast<uint16_t>(seq - max_seq_);

        if (probation_ > 0) {
            if (seq == static_cast<uint16_t>(max_seq_ + 1)) {
                --probation_;
                max_seq_ = seq;
                if (probation_ == 0) {
                    restart(seq);
                    ++received_;
                    return true;
                }
            } else {
                probation_ = MIN_SEQUENTIAL - 1;
                max_seq_ = seq;
            }
            return false;
        }

        if (delta < MAX_DROPOUT) {
            // In order, possibly after a gap.
            if (seq < max_seq_) {
                cycles_ += SEQ_MOD;
            }
            max_seq_ = seq;
        } else if (delta <= SEQ_MOD - MAX_MISORDER) {
            // A large jump: believe it only if the next packet follows on.
            if (seq != bad_seq_) {
                bad_seq_ = (uint32_t{seq} + 1) & (SEQ_MOD - 1);
                return false;
            }
            restart(seq);
        }
        // Otherwise a duplicate or late packet: counted, high-water mark unchanged.
        ++received_;
        return true;
    }

    [[nodiscard]] constexpr auto validated() const noexcept -> bool { return started_ && probation_ == 0; }

    /// The highest sequence number seen, extended with the wrap count.
    [[nodiscard]] constexpr auto extended_max() const noexcept -> uint32_t { return cycles_ + max_seq_; }

    /// Packets expected since the source was validated or restarted; 0 before that.
    [[nodiscard]] constexpr auto expected() const noexcept -> uint32_t { return validated() ? extended_max() - base_seq_ + 1 : 0; }

    /// Packets accounted for, duplicates included (as RFC 3550 counts them).
    [[nodiscard]] constexpr auto received() const noexcept -> uint32_t { return received_; }

    /// Cumulative loss; negative when duplicates outnumber losses.
    [[nodiscard]] constexpr auto lost() const noexcept -> int64_t
    {
        return static_cast<int64_t>(expected()) - static_cast<int64_t>(received_);
    }

    /// RFC 3550 §6.4.1 reception figures, as a receiver report carries them.
    struct LossReport
    {
        uint8_t fraction_lost;    // over the interval since the last report, in 1/256ths
        int32_t cumulative_lost;  // clamped to the report's 24-bit signed field
    };

    /// The figures for the interval since the previous call (or since validation).
    constexpr auto report() noexcept -> LossReport
    {
        auto const interval = take_interval();
        auto const lost_interval = static_cast<int64_t>(interval.expected) - static_cast<int64_t>(interval.received);
        uint8_t const fraction =
            (interval.expected == 0 || lost_interval <= 0) ? 0 : static_cast<uint8_t>((lost_interval << 8) / interval.expected);
        return {
            .fraction_lost = fraction, .cumulative_lost = static_cast<int32_t>(std::clamp<int64_t>(lost(), -0x800000, 0x7FFFFF))};
    }

  private:
    static constexpr uint32_t SEQ_MOD = 1U << 16U;

    /// Packets expected and received, as running totals or over an interval.
    struct Counts
    {
        uint32_t expected{0};
        uint32_t received{0};
    };

    /// The counts since the previous call, and starts the next interval (RFC 3550 Appendix A.3).
    [[nodiscard]] constexpr auto take_interval() noexcept -> Counts
    {
        Counts const now{.expected = expected(), .received = received_};
        Counts const interval{.expected = now.expected - prior_.expected, .received = now.received - prior_.received};
        prior_ = now;
        return interval;
    }

    constexpr void restart(uint16_t const seq) noexcept
    {
        base_seq_ = seq;
        max_seq_ = seq;
        bad_seq_ = SEQ_MOD + 1;  // matches no sequence number
        cycles_ = 0;
        received_ = 0;
        prior_ = {};
    }

    bool started_{false};
    uint16_t max_seq_{0};
    uint32_t probation_{MIN_SEQUENTIAL};
    uint32_t cycles_{0};
    uint32_t base_seq_{0};
    uint32_t bad_seq_{SEQ_MOD + 1};
    uint32_t received_{0};
    Counts prior_{};  ///< the totals at the previous report
};

}  // namespace statusbar::rtp
