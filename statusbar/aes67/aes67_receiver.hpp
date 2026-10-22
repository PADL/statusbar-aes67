#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An AES67 receiver: one RTP stream described by SDP, its frames handed on by
/// PTP sample index.
///
/// configure() takes the stream from a session's media: group and port,
/// payload type, encoding, channels and rate (which must be the receiver's
/// format; the ptime need not match, any whole number of frames is taken),
/// an RFC 4570 source filter if any, and the RFC 7273 direct media clock
/// offset. A stream without a direct media clock is refused: its timestamps
/// are not PTP time. drain() then, each tick, takes what the transport has
/// received: packets of the stream's payload type are accepted, the first
/// one's SSRC is learnt and other sources dropped (until the learnt one has
/// been silent for a second), the sequence is tracked (rtp::RtpSequence; a new
/// source's probation packets are handed on too, only an unconfirmed jump of a
/// validated source is dropped), and the RTP timestamp
/// becomes a sample index with PtpMediaClock::index_of_rtp() about the current
/// index. The payload goes to the FrameHandler as is.
///
/// Counters: packets and frames handed on; `lost`, frames in sequence gaps
/// not (yet) filled; `late`, frames that arrived out of order (after a later
/// packet), which are still handed on; `dropped`, packets refused (malformed,
/// wrong payload type, source or size, duplicates, or too old to place).
///
/// Threading: configure() and clear() make and close sockets: call them off
/// the real-time thread. drain() and the accessors are real-time safe.

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
    /// (the media clock's index at the current PTP time).
    void drain(uint64_t now_index) noexcept;

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
};

}  // namespace statusbar::aes67
