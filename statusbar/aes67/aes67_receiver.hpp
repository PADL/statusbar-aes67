#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An AES67 receiver: one RTP stream described by SDP, with an RFC 7273 direct media clock, its
/// payloads handed to a FrameHandler by PTP sample index. configure() and clear() make and close
/// sockets, off the real-time thread; drain() and the accessors are real-time safe.

#include "statusbar/aes67/aes67_stream.hpp"
#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/rtp/rtp_sequence.hpp"
#include "statusbar/sdp/sdp.hpp"
#include "statusbar/status/status.hpp"

#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace statusbar::aes67 {

class Receiver
{
  public:
    /// Datagrams taken from the transport per receive() call.
    static constexpr size_t BATCH = 32;
    /// receive() calls per drain() at most, so one tick's work is bounded.
    static constexpr size_t MAX_BATCHES = 4;

    Receiver(StreamFormat const& format, FrameHandler handler, Transport& transport) noexcept;

    /// Receive the stream `session.media[media_index]`, replacing any before.
    [[nodiscard]] auto configure(sdp::Session const& session, size_t media_index = 0) -> Status;
    /// Stop receiving: leave the group, forget the stream.
    void clear() noexcept;

    /// Take what has arrived and hand it on, with indices about `now_index`
    /// (the media clock's index at the current PTP time), behind the due-tick gate.
    void drain(uint64_t now_index) noexcept;
    /// Port receive() calls made and ticks skipped by the due-tick gate.
    [[nodiscard]] auto polls() const noexcept -> uint64_t { return polls_; }
    [[nodiscard]] auto skipped_polls() const noexcept -> uint64_t { return skipped_polls_; }

    [[nodiscard]] auto format() const noexcept -> StreamFormat const& { return format_; }
    [[nodiscard]] auto state() const noexcept -> State { return state_; }
    [[nodiscard]] auto counters() const noexcept -> Counters const& { return counters_; }
    /// Why the transport refused the last configure(), for State::error.
    [[nodiscard]] auto error() const noexcept -> std::error_code { return error_; }
    [[nodiscard]] auto configured() const noexcept -> bool { return port_ != nullptr; }
    /// The SSRC learnt from the stream's first packet.
    [[nodiscard]] auto ssrc() const noexcept -> std::optional<uint32_t> { return ssrc_; }

  private:
    void accept(Datagram const& datagram, uint64_t now_index) noexcept;
    /// Account a validated packet's place in the sequence; false to drop it.
    auto place(uint16_t sequence, uint32_t frames) noexcept -> bool;
    void reset_stream() noexcept;

    StreamFormat format_;
    FrameHandler handler_;
    Transport& transport_;
    std::unique_ptr<ReceivePort> port_;

    uint8_t payload_type_{0};
    uint32_t offset_{0};
    std::optional<ip::IPv4Address> source_filter_;

    std::optional<uint32_t> ssrc_;
    uint64_t last_index_{0};  ///< now_index when the learnt source was last heard
    rtp::RtpSequence sequence_;
    bool have_max_{false};
    uint16_t max_seq_{0};
    /// Bit i: max_seq_ - i received, for i up to RtpSequence::MAX_MISORDER.
    std::bitset<128> window_;
    Counters counters_;
    State state_{State::idle};
    std::error_code error_;

    /// The due-tick gate: the port is not asked before next_poll_index_ (0:
    /// every tick), unless the clock went back past gate_set_at_.
    uint64_t next_poll_index_{0};
    uint64_t gate_set_at_{0};
    uint64_t polls_{0};
    uint64_t skipped_polls_{0};
    uint64_t newest_end_{0};     ///< this drain: the newest packet's end index
    uint32_t newest_frames_{0};  ///< and its frames (0: none accepted)
};

}  // namespace statusbar::aes67
