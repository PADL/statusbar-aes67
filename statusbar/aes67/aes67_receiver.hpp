#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An AES67 receiver: one RTP stream described by SDP, with an RFC 7273 direct media clock (or a
/// sender clock whose offset its sender reports give), its payloads handed to a FrameHandler by sample
/// index. configure() and clear() make and close sockets, off the real-time thread; drain() and the
/// accessors are real-time safe.

#include "statusbar/aes67/aes67_stream.hpp"
#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/rtp/rtp_header.hpp"
#include "statusbar/rtp/rtp_sequence.hpp"
#include "statusbar/sdp/sdp.hpp"
#include "statusbar/status/status.hpp"

#include <atomic>
#include <bitset>
#include <cstdint>
#include <expected>
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

    /// Take only `mediaclk:sender` streams (true; others fail not_sender_clocked) instead of only
    /// `direct` ones (false, the default; others fail not_ptp_locked). Call before configure().
    void set_sender_clock(bool const sender) noexcept { sender_clock_ = sender; }
    [[nodiscard]] auto sender_clock() const noexcept -> bool { return sender_clock_; }

    /// Receive the stream `session.media[media_index]`, replacing any before. A direct stream's offset
    /// is the SDP's; a sender-clocked stream has none until set_offset().
    [[nodiscard]] auto configure(sdp::Session const& session, size_t media_index = 0) -> Status;
    /// The RTP offset (timestamp minus sample index, mod 2^32) drain() places packets by, replacing
    /// any before; a sender-clocked stream's packets are learnt but dropped until it is set, and a new
    /// source clears it. Any thread.
    void set_offset(uint32_t const offset) noexcept { offset_.store(OFFSET_SET | offset, std::memory_order_release); }
    /// The offset in force, or nullopt while a sender-clocked stream waits for set_offset(). Any thread.
    [[nodiscard]] auto offset() const noexcept -> std::optional<uint32_t>
    {
        uint64_t const value = offset_.load(std::memory_order_acquire);
        return (value & OFFSET_SET) != 0 ? std::optional{static_cast<uint32_t>(value)} : std::nullopt;
    }
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
    /// Every counter to zero; the stream, its source and sequence go on. The draining thread.
    void reset_counters() noexcept { counters_ = {}; }
    /// Why the transport refused the last configure(), for State::error.
    [[nodiscard]] auto error() const noexcept -> std::error_code { return error_; }
    [[nodiscard]] auto configured() const noexcept -> bool { return port_ != nullptr; }
    /// The SSRC learnt from the stream's first packet.
    [[nodiscard]] auto ssrc() const noexcept -> std::optional<uint32_t> { return ssrc_; }

    /// Where this stream's RTCP goes and arrives (the SDP's a=rtcp, else port + 1), marked `dscp`:
    /// the DSCP this host would send a similar stream with (AES67-2018 Section 6.2). Fails
    /// not_configured, rtcp_multiplexed or no_rtcp_peer. Off the real-time thread.
    [[nodiscard]] auto rtcp_options(uint8_t dscp = rtp::FrameAddressing::DSCP_MEDIA) const -> StatusValue<RtcpOptions>;
    /// The latest ReceiveState, published by drain(). Any thread.
    [[nodiscard]] auto receive_state() const noexcept -> ReceiveState { return receive_state_.load(); }

  private:
    static constexpr uint64_t OFFSET_SET = uint64_t{1} << 32U;

    /// `offset` is offset_ as loaded: a packet is placed only when OFFSET_SET is in it.
    void accept(Datagram const& datagram, uint64_t now_index, uint64_t offset) noexcept;
    /// Account a validated packet's place in the sequence; false to drop it.
    auto place(uint16_t sequence, uint32_t frames) noexcept -> bool;
    void reset_stream() noexcept;
    /// From configure(), the media validated: open the port and set the stream up from the SDP.
    [[nodiscard]] auto open_stream(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection)
        -> Status;
    /// The stream's payload type, offset, source filter and RTCP endpoints, from the SDP.
    void apply_media(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection);
    /// clear(), then open the transport for `destination`; on failure State::error and error().
    [[nodiscard]] auto reopen(Endpoint const& destination) -> Status;
    /// offset_'s value for a stream with `clock`: a direct clock's offset, flagged set; 0 when sender-clocked.
    [[nodiscard]] auto initial_offset(std::optional<sdp::MediaClock> const& clock) const noexcept -> uint64_t;
    /// From configure(): the RTCP endpoints, or why there are none.
    void set_rtcp(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection);
    /// The RTCP endpoints for the media (RFC 3605), without the DSCP; fails rtcp_multiplexed or no_rtcp_peer.
    [[nodiscard]] auto rtcp_options_for(
        sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection) const -> StatusValue<RtcpOptions>;
    /// Receives and accepts up to MAX_BATCHES batches; true when every batch came back full (more may wait).
    [[nodiscard]] auto receive_batches(uint64_t now_index) noexcept -> bool;
    /// Sets the due-tick gate after a drain: open after a backlog, else a tick before the next packet is due.
    void set_gate(uint64_t now_index, bool backlog) noexcept;
    /// Counts a refused packet: dropped, and its reason.
    void drop(DropReason reason) noexcept;
    /// The packet's frames when it is this stream's, from its source and in sequence; else why not.
    [[nodiscard]] auto admit(rtp::RtpPacket const& packet, ip::IPv4Address const& source, uint64_t now_index) noexcept
        -> std::expected<uint32_t, DropReason>;
    /// Why the packet's payload type, size or source is not this stream's; nullopt when it is.
    [[nodiscard]] auto check_payload(rtp::RtpPacket const& packet, ip::IPv4Address const& source) const noexcept
        -> std::optional<DropReason>;
    /// Learns the SSRC, or adopts a new one after the learnt source fell silent; else why the packet is dropped.
    [[nodiscard]] auto admit_source(uint32_t ssrc, uint64_t now_index) noexcept -> std::expected<void, DropReason>;
    /// Validates the sequence number (RFC 3550 Appendix A.1) and places it; false to drop the packet.
    [[nodiscard]] auto admit_sequence(uint16_t sequence, uint32_t frames) noexcept -> bool;
    /// Notes an accepted packet's arrival: the interarrival jitter (RFC 3550 Appendix A.8) and its timestamp.
    void note_arrival(uint32_t timestamp, uint64_t now_index) noexcept;
    /// Hands a placed packet to the handler at its sample index and counts it.
    void deliver(rtp::RtpPacket const& packet, uint32_t frames, uint64_t offset, uint64_t now_index) noexcept;
    /// Restarts the reorder window at `sequence`.
    void start_window(uint16_t sequence) noexcept;
    /// Moves the window `ahead` to `sequence`, counting the packets skipped as lost.
    void advance_window(uint16_t sequence, uint16_t ahead, uint32_t frames) noexcept;
    /// Marks a late packet `behind` the newest; false for a duplicate.
    [[nodiscard]] auto mark_late(uint16_t behind, uint32_t frames) noexcept -> bool;

    StreamFormat format_;
    FrameHandler handler_;
    Transport& transport_;
    std::unique_ptr<ReceivePort> port_;

    uint8_t payload_type_{0};
    std::atomic<uint64_t> offset_{0};  ///< OFFSET_SET | offset, or 0 for none yet
    bool sender_clock_{false};
    std::optional<ip::IPv4Address> source_filter_;
    std::optional<RtcpOptions> rtcp_;  ///< from configure(), without the DSCP
    std::error_code rtcp_error_;       ///< why there is none

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
    /// What the current drain has seen.
    struct DrainState
    {
        uint64_t newest_end{0};     ///< the newest packet's end index
        uint32_t newest_frames{0};  ///< and its frames (0: none placed)
        bool heard{false};          ///< a packet of the learnt source was accepted
    };
    DrainState this_drain_{};

    // RFC 3550 Appendix A.8 interarrival jitter, and what drain() publishes.
    uint32_t jitter_x16_{0};
    uint32_t last_transit_{0};
    uint64_t last_arrival_index_{0};
    bool have_transit_{false};
    uint32_t last_rtp_ts_{0};
    itc::SeqlockValue<ReceiveState> receive_state_;  ///< real-time -> RTCP session
};

}  // namespace statusbar::aes67
