// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/aes67/aes67_receiver.hpp"

#include "padl/aes67/aes67_error.hpp"
#include "padl/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/net/net_socket.hpp"

#include <algorithm>
#include <array>

namespace padl::aes67 {

namespace {

/// The media is one a receiver of `format` accepts: a `sender` media clock when `sender_clock`, else
/// `direct` (RFC 7273 Section 5), at a rate the media clock supports, in the format.
[[nodiscard]] auto check_media(
    sdp::Session const& session, sdp::Media const& media, StreamFormat const& format, bool const sender_clock) -> statusbar::Status
{
    auto const clock = session.effective_media_clock(media);
    auto const kind = clock ? clock->kind() : sdp::MediaClockKind::other;
    if (sender_clock && kind != sdp::MediaClockKind::sender) {
        return statusbar::failure(Aes67Error::not_sender_clocked);
    }
    if (!sender_clock && kind != sdp::MediaClockKind::direct) {
        return statusbar::failure(Aes67Error::not_ptp_locked);
    }
    if (!mediaclock::PtpMediaClock::supports(media.sample_rate)) {
        return statusbar::failure(Aes67Error::unsupported_format);
    }
    return check_media_format(media, format);
}

/// The source a filter admits when it applies to `destination` (RFC 4570), else none.
[[nodiscard]] auto matching_source(
    std::optional<sdp::SourceFilter> const& filter, statusbar::ip::IPv4Address const& destination) noexcept
    -> std::optional<statusbar::ip::IPv4Address>
{
    if (filter && (!filter->destination || *filter->destination == destination)) {
        return filter->source;
    }
    return std::nullopt;
}

/// The most frames a packet of `media` holds: its ptime's, or its maxptime's when longer (RFC 8866 Sections 6.4, 6.5).
[[nodiscard]] auto max_frames_for(sdp::Media const& media) noexcept -> uint32_t
{
    uint64_t const us = std::max(media.packet_time_us, media.max_packet_time_us.value_or(0));
    return static_cast<uint32_t>(std::max<uint64_t>(((us * media.sample_rate) + 999'999) / 1'000'000, 1));
}

}  // namespace

auto receive_endpoint(
    sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection, Transport const& transport)
    -> statusbar::StatusValue<Endpoint>
{
    auto const& address = connection.address;
    if (address.is_multicast() || (!address.is_unspecified() && transport.is_local_address(address))) {
        return Endpoint{.address = address, .port = media.port};
    }
    if (!address.is_unspecified() && statusbar::ip::parse_ipv4(session.origin_address) == address) {
        return Endpoint{.address = transport.local_address(), .port = media.port};
    }
    return statusbar::failure(Aes67Error::foreign_destination);
}

Receiver::Receiver(StreamFormat const& format, FrameHandler handler, Transport& transport) noexcept
    : format_{format}
    , handler_{std::move(handler)}
    , transport_{transport}
    , depacketizer_{format}
{}

auto Receiver::configure(sdp::Session const& session, size_t const media_index) -> statusbar::Status
{
    auto const media = select_media(session, media_index, format_.encoding);
    if (!media) {
        return statusbar::forward_failure(media);
    }
    auto const connection = session.effective_connection(*media);
    if (!connection) {
        return statusbar::failure(Aes67Error::no_connection);
    }
    if (auto const checked = check_media(session, *media, format_, sender_clock()); !checked) {
        return checked;
    }
    return open_stream(session, *media, *connection);
}

auto Receiver::open_stream(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection)
    -> statusbar::Status
{
    auto const endpoint = receive_endpoint(session, media, connection, transport_);
    if (auto const opened = endpoint
            ? reopen(*endpoint, matching_source(session.effective_source_filter(media), connection.address))
            : refused(endpoint.error());
        !opened) {
        return opened;
    }
    apply_media(session, media, connection, *endpoint);
    state_ = State::ready;
    error_.clear();
    return statusbar::success();
}

void Receiver::apply_media(
    sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection, Endpoint const& endpoint)
{
    depacketizer_.set_stream(media.payload_type, max_frames_for(media));
    offset_.store(initial_offset(session.effective_media_clock(media)), std::memory_order_release);
    set_source_filter(session, media, connection, endpoint);
    set_rtcp(session, media, connection, endpoint);
}

void Receiver::set_source_filter(
    sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection, Endpoint const& endpoint)
{
    auto source_filter = matching_source(session.effective_source_filter(media), connection.address);
    if (!source_filter && endpoint.address != connection.address) {
        source_filter = connection.address;  // an offer: c= is the sender (AES67-2023 Clause 8.5.2)
    }
    depacketizer_.set_source_filter(source_filter);
    receive_state_.store({.source = source_filter.value_or(statusbar::ip::IPv4Address{})});
}

auto Receiver::reopen(Endpoint const& destination, std::optional<statusbar::ip::IPv4Address> const& source) -> statusbar::Status
{
    clear();
    receive_state_.store({});  // not drained while cleared, so drain() is not storing too
    bool const specific = source && statusbar::net::is_source_specific_group(destination.address);
    return adopt(specific ? transport_.open_receive_from(destination, *source) : transport_.open_receive(destination));
}

auto Receiver::adopt(statusbar::StatusValue<std::unique_ptr<ReceivePort>> port) -> statusbar::Status
{
    if (!port) {
        return refused(port.error());
    }
    port_ = std::move(*port);
    return statusbar::success();
}

auto Receiver::refused(std::error_code const error) noexcept -> statusbar::Status
{
    state_ = State::error;
    error_ = error;
    return statusbar::failure(error);
}

auto Receiver::initial_offset(std::optional<sdp::MediaClock> const& clock) const noexcept -> uint64_t
{
    return !sender_clock() && clock && clock->direct_offset ? OFFSET_SET | *clock->direct_offset : 0;
}

void Receiver::set_rtcp(
    sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection, Endpoint const& endpoint)
{
    auto const options = rtcp_options_for(session, media, connection, endpoint);
    rtcp_.reset();
    rtcp_error_.clear();
    if (options) {
        rtcp_ = *options;
    } else {
        rtcp_error_ = options.error();
    }
}

auto Receiver::rtcp_options_for(
    sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection, Endpoint const& received) const
    -> statusbar::StatusValue<RtcpOptions>
{
    if (media.rtcp_mux) {
        return statusbar::failure(Aes67Error::rtcp_multiplexed);
    }
    Endpoint const endpoint = media.rtcp
        ? Endpoint{.address = media.rtcp->address.value_or(received.address), .port = media.rtcp->port}
        : rtcp_endpoint_for(received);
    RtcpOptions options{
        .destination = endpoint,
        .local = {},
        .ttl = connection.ttl != 0 ? connection.ttl : sdp::Connection::DEFAULT_MULTICAST_TTL,
    };
    if (media.rtcp && media.rtcp->address && !endpoint.address.is_multicast()) {
        // RFC 3605: the address is where reports go; they arrive here at the same port.
        options.local = {
            .address = received.address.is_multicast() ? statusbar::ip::IPv4Address{} : received.address, .port = endpoint.port};
    } else if (!endpoint.address.is_multicast()) {
        // A unicast stream: reports arrive at this address and go to the sender, at the same port.
        auto const& filter = depacketizer_.source_filter();
        auto const peer = filter ? filter : statusbar::ip::parse_ipv4(session.origin_address);
        if (!peer) {
            return statusbar::failure(Aes67Error::no_rtcp_peer);
        }
        options.local = endpoint;
        options.destination = {.address = *peer, .port = endpoint.port};
    }
    options.source =
        statusbar::net::is_source_specific_group(options.destination.address) ? depacketizer_.source_filter() : std::nullopt;
    return options;
}

auto Receiver::rtcp_options(uint8_t const dscp) const -> statusbar::StatusValue<RtcpOptions>
{
    if (!port_) {
        return statusbar::failure(Aes67Error::not_configured);
    }
    if (!rtcp_) {
        return statusbar::failure(rtcp_error_);
    }
    RtcpOptions options = *rtcp_;
    options.dscp = dscp;
    return options;
}

void Receiver::clear() noexcept
{
    port_.reset();
    depacketizer_.set_source_filter(std::nullopt);
    rtcp_.reset();
    reset_stream();
    offset_.store(0, std::memory_order_release);
    state_ = State::idle;
}

void Receiver::reset_stream() noexcept
{
    next_poll_index_ = 0;
    depacketizer_.reset();
}

void Receiver::drain(uint64_t const now_index) noexcept
{
    if (!port_) {
        return;
    }
    // The due-tick gate (UDP): skip until a packet can be there, unless the
    // clock stepped back past the gate's horizon.
    if (next_poll_index_ != 0 && now_index < next_poll_index_ && now_index >= gate_set_at_ && !transport_.drain_every_tick()) {
        return;
    }
    bool const backlog = receive_batches(now_index);
    notify_port_error();
    if (this_drain_.accepted && depacketizer_.ssrc()) {
        receive_state_.store(depacketizer_.receive_state(now_index));
    }
    set_gate(now_index, backlog);
}

void Receiver::notify_port_error() noexcept
{
    if (auto const error = port_->error()) {
        state_ = State::error;
        error_ = error;
        return;
    }
    if (state_ == State::error) {
        state_ = State::ready;  // a stream afresh from its next packet, as after configure()
        error_.clear();
        reset_stream();
    }
}

auto Receiver::receive_batches(uint64_t const now_index) noexcept -> bool
{
    this_drain_ = {};
    uint64_t offset = offset_.load(std::memory_order_acquire);
    for (size_t round = 0; round < MAX_BATCHES; ++round) {
        size_t const n = port_->receive(batch_);
        for (size_t i = 0; i < n; ++i) {
            if (depacketizer_.sender_clock()) {
                offset = offset_.load(std::memory_order_acquire);  // set, or cleared by a new source
            }
            if (!is_multiplexed_rtcp(batch_[i].payload)) {
                accept(batch_[i], now_index, offset);  // the stream's own RTCP is not a refused packet
            }
        }
        if (n < batch_.size()) {
            return false;
        }
    }
    return true;
}

void Receiver::set_gate(uint64_t const now_index, bool const backlog) noexcept
{
    if (depacketizer_.take_restarted()) {
        next_poll_index_ = 0;  // the drain adopted a new source: as reset_stream()
    }
    if (backlog) {
        next_poll_index_ = 0;  // the rest next tick
    } else if (this_drain_.newest_frames != 0) {
        // The next packet, [end, end + n), is sent no earlier than end + n:
        // ask again a tick before that, but never later than a packet time
        // and a tick from now.
        uint64_t const tick = (format_.sample_rate + 7999) / 8000;
        uint64_t const due = this_drain_.newest_end + this_drain_.newest_frames -
            std::min<uint64_t>(tick, this_drain_.newest_end + this_drain_.newest_frames);
        next_poll_index_ = std::min(due, now_index + this_drain_.newest_frames + tick);
        gate_set_at_ = now_index;
    }
    // (Nothing came: the gate stays open, every tick until a packet does.)
}

void Receiver::accept(Datagram const& datagram, uint64_t const now_index, uint64_t const offset) noexcept
{
    auto const packet = rtp::parse_rtp(datagram.payload);
    auto const frames =
        packet ? depacketizer_.admit(*packet, datagram.source.address, now_index) : std::unexpected{DropReason::not_rtp};
    if (!packet || !frames) {
        refuse(frames.error());
        return;
    }
    notify_arrival(packet->header.timestamp.get(), now_index);
    if ((offset & OFFSET_SET) == 0) {
        depacketizer_.drop(DropReason::no_offset);  // learnt, but not placed before the sender's first report
        return;
    }
    deliver(*packet, *frames, offset, now_index);
}

void Receiver::refuse(DropReason const reason) noexcept
{
    if (reason == DropReason::no_offset) {
        offset_.store(0, std::memory_order_release);  // a sender-clocked stream's new source: its reports give its own
    }
    depacketizer_.drop(reason);
}

void Receiver::notify_arrival(uint32_t const timestamp, uint64_t const now_index) noexcept
{
    depacketizer_.notify_arrival(timestamp, now_index);
    this_drain_.accepted = true;
}

void Receiver::deliver(
    rtp::RtpPacket const& packet, uint32_t const frames, uint64_t const offset, uint64_t const now_index) noexcept
{
    uint64_t const index =
        mediaclock::PtpMediaClock::index_of_rtp(packet.header.timestamp.get(), static_cast<uint32_t>(offset), now_index);
    if (index + frames > this_drain_.newest_end) {
        this_drain_.newest_end = index + frames;
        this_drain_.newest_frames = frames;
    }
    if (handler_) {
        handler_(index, frames, packet.payload);
    }
    depacketizer_.delivered(frames);
    state_ = State::running;
}

}  // namespace padl::aes67
