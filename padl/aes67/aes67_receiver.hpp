#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An AES67 receiver: one RTP stream described by SDP, with an RFC 7273 direct media clock (or a
/// sender clock whose offset its sender reports give), its payloads handed to a FrameHandler by sample
/// index. configure() and clear() make and close sockets, off the real-time thread; drain() and the
/// accessors are real-time safe.

#include "padl/aes67/aes67_depacketizer.hpp"
#include "padl/aes67/aes67_stream.hpp"
#include "padl/aes67/aes67_transport.hpp"
#include "padl/rtp/rtp_header.hpp"
#include "padl/sdp/sdp_session.hpp"
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace padl::aes67 {

/// Where a stream on `connection` arrives over `transport`: a group, or this host's address, as `c=` names it
/// (RFC 8866 Section 5.7); `transport.local_address()` when `c=` is the session's origin, the sender (AES67-2023
/// Clause 8.5.2, an offer). Any other unicast `c=`, 0.0.0.0 among them, fails foreign_destination.
[[nodiscard]] auto receive_endpoint(
    sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection, Transport const& transport)
    -> statusbar::StatusValue<Endpoint>;

/// The plan of a receiver of `format` for `media` on `connection` over `transport`, `sender_clock` or direct
/// (Receiver::set_sender_clock()): where receive_endpoint() says, joined and filtered by the SDP's source filter
/// (RFC 4570) or an offer's sender. Fails as receive_endpoint().
[[nodiscard]] auto receive_plan(
    sdp::Session const& session,
    sdp::Media const& media,
    sdp::Connection const& connection,
    StreamFormat const& format,
    bool sender_clock,
    Transport const& transport) -> statusbar::StatusValue<ReceivePlan>;

class Receiver
{
  public:
    /// Datagrams read from the transport per receive() call.
    static constexpr size_t BATCH = 32;
    /// receive() calls per drain() at most, so one tick's work is bounded.
    static constexpr size_t MAX_BATCHES = 4;

    Receiver(StreamFormat const& format, FrameHandler handler, Transport& transport) noexcept;

    /// Accept only `mediaclk:sender` streams (true; others fail not_sender_clocked) instead of only
    /// `direct` ones (false, the default; others fail not_ptp_locked). Call before configure().
    void set_sender_clock(bool const sender) noexcept { depacketizer_.set_sender_clock(sender); }
    [[nodiscard]] auto sender_clock() const noexcept -> bool { return depacketizer_.sender_clock(); }

    /// Receive the stream `session.media[media_index]`, replacing any before. A direct stream's offset
    /// is the SDP's; a sender-clocked stream has none until set_offset().
    [[nodiscard]] auto configure(sdp::Session const& session, size_t media_index = 0) -> statusbar::Status;
    /// The RTP offset (timestamp minus sample index, mod 2^32) drain() places packets by, replacing
    /// any before; a sender-clocked stream's packets are learnt but dropped until it is set, and a new
    /// source clears it. Any thread.
    void set_offset(uint32_t const offset) noexcept { offset_.store(OFFSET_SET | offset, std::memory_order_release); }
    /// Forget the offset: a sender-clocked stream's packets are dropped until set_offset() again. Any thread.
    void clear_offset() noexcept { offset_.store(0, std::memory_order_release); }
    /// The offset in force, or nullopt while a sender-clocked stream waits for set_offset(). Any thread.
    [[nodiscard]] auto offset() const noexcept -> std::optional<uint32_t>
    {
        uint64_t const value = offset_.load(std::memory_order_acquire);
        return (value & OFFSET_SET) != 0 ? std::optional{static_cast<uint32_t>(value)} : std::nullopt;
    }
    /// Stop receiving: leave the group, forget the stream.
    void clear() noexcept;

    /// Read what has arrived and hand it on, with indices about `now_index`
    /// (the media clock's index at the current PTP time), behind the due-tick gate.
    void drain(uint64_t now_index) noexcept;

    [[nodiscard]] auto format() const noexcept -> StreamFormat const& { return format_; }
    [[nodiscard]] auto state() const noexcept -> State { return state_; }
    [[nodiscard]] auto counters() const noexcept -> Counters const& { return depacketizer_.counters(); }
    /// Every counter to zero; the stream, its source and sequence go on. The draining thread.
    void reset_counters() noexcept { depacketizer_.reset_counters(); }
    /// Why the transport refused the last configure(), or (since the last drain()) why its port cannot
    /// receive (ReceivePort::error()), for State::error.
    [[nodiscard]] auto error() const noexcept -> std::error_code { return error_; }
    [[nodiscard]] auto configured() const noexcept -> bool { return port_ != nullptr; }
    /// The SSRC learnt from the stream's first packet.
    [[nodiscard]] auto ssrc() const noexcept -> std::optional<uint32_t> { return depacketizer_.ssrc(); }

    /// Where this stream's RTCP goes and arrives (the SDP's a=rtcp, else port + 1), marked `dscp`:
    /// the DSCP this host would send a similar stream with (AES67-2023 Clause 6.2). Fails
    /// not_configured, rtcp_multiplexed or no_rtcp_peer. Off the real-time thread.
    [[nodiscard]] auto rtcp_options(uint8_t dscp = rtp::FrameAddressing::DSCP_MEDIA) const -> statusbar::StatusValue<RtcpOptions>;
    /// The latest ReceiveState, published by drain(). Any thread.
    [[nodiscard]] auto receive_state() const noexcept -> ReceiveState { return receive_state_.load(); }

  private:
    static constexpr uint64_t OFFSET_SET = uint64_t{1} << 32U;

    /// `offset` is offset_ as loaded: a packet is placed only when OFFSET_SET is in it.
    void accept(Datagram const& datagram, uint64_t now_index, uint64_t offset) noexcept;
    /// Forgets the stream's source and sequence, and opens the due-tick gate.
    void reset_stream() noexcept;
    /// From configure(), the media validated: open the port receive_plan() plans and set the stream up from the SDP; on
    /// failure State::error and error().
    [[nodiscard]] auto open_stream(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection)
        -> statusbar::Status;
    /// The plan's payload type, packet size, offset and source filter, the filter published as ReceiveState::source
    /// until a source is learnt; and the RTCP endpoints from the SDP.
    void apply_media(
        sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection, ReceivePlan const& plan);
    /// clear(), then open the transport for `plan` (Transport::open_receive_stream()); on failure State::error and error().
    [[nodiscard]] auto reopen(ReceivePlan const& plan) -> statusbar::Status;
    /// State::error with `error`, returned.
    [[nodiscard]] auto refused(std::error_code error) noexcept -> statusbar::Status;
    /// Adopts the port reopen() opened, or records why it did not open as State::error and error().
    [[nodiscard]] auto adopt(statusbar::StatusValue<std::unique_ptr<ReceivePort>> port) -> statusbar::Status;
    /// From configure(): the RTCP endpoints, or why there are none.
    void set_rtcp(
        sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection, Endpoint const& endpoint);
    /// The RTCP endpoints for the media received at `received` (RFC 3605), without the DSCP; fails rtcp_multiplexed
    /// or no_rtcp_peer.
    [[nodiscard]] auto rtcp_options_for(
        sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection, Endpoint const& received) const
        -> statusbar::StatusValue<RtcpOptions>;
    /// State::error while the port reports one (ReceivePort::error()), ready again once it does not.
    void notify_port_error() noexcept;
    /// Receives and accepts up to MAX_BATCHES batches; true when every batch came back full (more may wait).
    [[nodiscard]] auto receive_batches(uint64_t now_index) noexcept -> bool;
    /// Sets the due-tick gate after a drain: open after a backlog or a new source, else a tick before the next packet is due.
    void set_gate(uint64_t now_index, bool backlog) noexcept;
    /// Counts a packet the depacketizer refused, forgetting the offset when it adopted a sender-clocked stream's new
    /// source (no_offset).
    void refuse(DropReason reason) noexcept;
    /// Notes an admitted packet's arrival (Depacketizer::notify_arrival()) for this drain.
    void notify_arrival(uint32_t timestamp, uint64_t now_index) noexcept;
    /// Hands a placed packet to the handler at its sample index and counts it.
    void deliver(rtp::RtpPacket const& packet, uint32_t frames, uint64_t offset, uint64_t now_index) noexcept;

    StreamFormat format_;
    FrameHandler handler_;
    Transport& transport_;
    std::unique_ptr<ReceivePort> port_;
    /// receive_batches()' datagrams, built once rather than value-initialised on every drain.
    std::array<Datagram, BATCH> batch_{};

    std::atomic<uint64_t> offset_{0};  ///< OFFSET_SET | offset, or 0 for none yet
    std::optional<RtcpOptions> rtcp_;  ///< from configure(), without the DSCP
    std::error_code rtcp_error_;       ///< why there is none

    Depacketizer depacketizer_;
    State state_{State::idle};
    std::error_code error_;

    /// The due-tick gate: the port is not asked before next_poll_index_ (0:
    /// every tick), unless the clock went back past gate_set_at_.
    uint64_t next_poll_index_{0};
    uint64_t gate_set_at_{0};
    /// What the current drain has seen.
    struct DrainState
    {
        uint64_t newest_end{0};     ///< the newest packet's end index
        uint32_t newest_frames{0};  ///< and its frames (0: none placed)
        bool accepted{false};       ///< a packet of the learnt source was accepted
    };
    DrainState this_drain_{};
    statusbar::itc::SeqlockValue<ReceiveState> receive_state_;  ///< real-time -> RTCP session
};

}  // namespace padl::aes67
