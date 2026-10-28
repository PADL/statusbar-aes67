// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_receiver.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"

#include <algorithm>
#include <array>

namespace statusbar::aes67 {

namespace {

/// Sequence numbers the reorder window remembers: as far back as RtpSequence
/// accepts a late packet.
constexpr size_t WINDOW = rtp::RtpSequence::MAX_MISORDER + 1;
static_assert(WINDOW <= 128);

/// The media is one a receiver of `format` takes: a `sender` media clock when `sender_clock`, else
/// `direct` (RFC 7273 Section 5), at a rate the media clock supports, in the format.
[[nodiscard]] auto check_media(
    sdp::Session const& session, sdp::Media const& media, StreamFormat const& format, bool const sender_clock) -> Status
{
    auto const clock = session.effective_media_clock(media);
    auto const kind = clock ? clock->kind() : sdp::MediaClockKind::other;
    if (sender_clock && kind != sdp::MediaClockKind::sender) {
        return failure(Aes67Error::not_sender_clocked);
    }
    if (!sender_clock && kind != sdp::MediaClockKind::direct) {
        return failure(Aes67Error::not_ptp_locked);
    }
    if (!mediaclock::PtpMediaClock::supports(media.sample_rate)) {
        return failure(Aes67Error::unsupported_format);
    }
    return check_media_format(media, format);
}

/// The source a filter admits when it applies to `destination` (RFC 4570), else none.
[[nodiscard]] auto matching_source(std::optional<sdp::SourceFilter> const& filter, ip::IPv4Address const& destination) noexcept
    -> std::optional<ip::IPv4Address>
{
    if (filter && (!filter->destination || *filter->destination == destination)) {
        return filter->source;
    }
    return std::nullopt;
}

}  // namespace

Receiver::Receiver(StreamFormat const& format, FrameHandler handler, Transport& transport) noexcept
    : format_{format}
    , handler_{std::move(handler)}
    , transport_{transport}
{}

auto Receiver::configure(sdp::Session const& session, size_t const media_index) -> Status
{
    auto const media = select_media(session, media_index, format_.encoding);
    if (!media) {
        return forward_failure(media);
    }
    auto const connection = session.effective_connection(*media);
    if (!connection) {
        return failure(Aes67Error::no_connection);
    }
    if (auto const checked = check_media(session, *media, format_, sender_clock_); !checked) {
        return checked;
    }
    return open_stream(session, *media, *connection);
}

auto Receiver::open_stream(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection) -> Status
{
    if (auto const opened = reopen({.address = connection.address, .port = media.port}); !opened) {
        return opened;
    }
    apply_media(session, media, connection);
    state_ = State::ready;
    error_.clear();
    return success();
}

void Receiver::apply_media(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection)
{
    payload_type_ = media.payload_type;
    offset_.store(initial_offset(session.effective_media_clock(media)), std::memory_order_release);
    source_filter_ = matching_source(session.effective_source_filter(media), connection.address);
    set_rtcp(session, media, connection);
}

auto Receiver::reopen(Endpoint const& destination) -> Status
{
    clear();
    receive_state_.store({});  // not drained while cleared, so drain() is not storing too
    auto port = transport_.open_receive(destination);
    if (!port) {
        state_ = State::error;
        error_ = port.error();
        return failure(port.error());
    }
    port_ = std::move(*port);
    return success();
}

auto Receiver::initial_offset(std::optional<sdp::MediaClock> const& clock) const noexcept -> uint64_t
{
    return !sender_clock_ && clock && clock->direct_offset ? OFFSET_SET | *clock->direct_offset : 0;
}

void Receiver::set_rtcp(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection)
{
    auto const options = rtcp_options_for(session, media, connection);
    rtcp_.reset();
    rtcp_error_.clear();
    if (options) {
        rtcp_ = *options;
    } else {
        rtcp_error_ = options.error();
    }
}

auto Receiver::rtcp_options_for(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection) const
    -> StatusValue<RtcpOptions>
{
    if (media.rtcp_mux) {
        return failure(Aes67Error::rtcp_multiplexed);
    }
    Endpoint const endpoint = media.rtcp
        ? Endpoint{.address = media.rtcp->address.value_or(connection.address), .port = media.rtcp->port}
        : rtcp_endpoint_of({.address = connection.address, .port = media.port});
    RtcpOptions options{
        .destination = endpoint,
        .local = {},
        .ttl = connection.ttl != 0 ? connection.ttl : sdp::Connection::DEFAULT_MULTICAST_TTL,
    };
    if (media.rtcp && media.rtcp->address && !endpoint.address.is_multicast()) {
        // RFC 3605: the address is where reports go; they arrive here at the same port.
        options.local = {
            .address = connection.address.is_multicast() ? ip::IPv4Address{} : connection.address, .port = endpoint.port};
    } else if (!endpoint.address.is_multicast()) {
        // A unicast stream: reports arrive at this address and go to the sender, at the same port.
        auto const peer = source_filter_ ? source_filter_ : ip::parse_ipv4(session.origin_address);
        if (!peer) {
            return failure(Aes67Error::no_rtcp_peer);
        }
        options.local = endpoint;
        options.destination = {.address = *peer, .port = endpoint.port};
    }
    return options;
}

auto Receiver::rtcp_options(uint8_t const dscp) const -> StatusValue<RtcpOptions>
{
    if (!port_) {
        return failure(Aes67Error::not_configured);
    }
    if (!rtcp_) {
        return failure(rtcp_error_);
    }
    RtcpOptions options = *rtcp_;
    options.dscp = dscp;
    return options;
}

void Receiver::clear() noexcept
{
    port_.reset();
    source_filter_.reset();
    rtcp_.reset();
    reset_stream();
    offset_.store(0, std::memory_order_release);
    state_ = State::idle;
}

void Receiver::reset_stream() noexcept
{  // NOLINT(statusbar-block-statements): one stream's state, reset together
    next_poll_index_ = 0;
    ssrc_.reset();
    sequence_ = rtp::RtpSequence{};
    have_max_ = false;
    max_seq_ = 0;
    window_.reset();
    jitter_x16_ = 0;
    have_transit_ = false;
}

void Receiver::drain(uint64_t const now_index) noexcept
{
    if (!port_) {
        return;
    }
    // The due-tick gate (UDP): skip until a packet can be there, unless the
    // clock stepped back past the gate's horizon.
    if (next_poll_index_ != 0 && now_index < next_poll_index_ && now_index >= gate_set_at_ && !transport_.drain_every_tick()) {
        ++skipped_polls_;
        return;
    }
    bool const backlog = receive_batches(now_index);
    if (this_drain_.heard && ssrc_) {
        receive_state_.store({
            .ssrc = *ssrc_,
            .valid = true,
            .extended_max = sequence_.extended_max(),
            .expected = sequence_.expected(),
            .received = sequence_.received(),
            .jitter_x16 = jitter_x16_,
            .last_rtp_ts = last_rtp_ts_,
            .last_now_index = now_index,
        });
    }
    set_gate(now_index, backlog);
}

auto Receiver::receive_batches(uint64_t const now_index) noexcept -> bool
{
    this_drain_ = {};
    uint64_t offset = offset_.load(std::memory_order_acquire);
    std::array<Datagram, BATCH> batch{};
    for (size_t round = 0; round < MAX_BATCHES; ++round) {
        size_t const n = port_->receive(batch);
        ++polls_;
        for (size_t i = 0; i < n; ++i) {
            if (sender_clock_) {
                offset = offset_.load(std::memory_order_acquire);  // set, or cleared by a new source
            }
            accept(batch[i], now_index, offset);
        }
        if (n < batch.size()) {
            return false;
        }
    }
    return true;
}

void Receiver::set_gate(uint64_t const now_index, bool const backlog) noexcept
{
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
    auto const frames = packet ? admit(*packet, datagram.source.address, now_index) : std::unexpected{DropReason::not_rtp};
    if (!packet || !frames) {
        drop(frames.error());
        return;
    }
    note_arrival(packet->header.timestamp.get(), now_index);
    if ((offset & OFFSET_SET) == 0) {
        drop(DropReason::no_offset);  // learnt, but not placed before the sender's first report
        return;
    }
    deliver(*packet, *frames, offset, now_index);
}

void Receiver::drop(DropReason const reason) noexcept
{
    ++counters_.dropped;
    ++counters_.drops[reason];
}

auto Receiver::admit(rtp::RtpPacket const& packet, ip::IPv4Address const& source, uint64_t const now_index) noexcept
    -> std::expected<uint32_t, DropReason>
{
    if (auto const refused = check_payload(packet, source)) {
        return std::unexpected{*refused};
    }
    if (auto const admitted = admit_source(packet.header.ssrc.get(), now_index); !admitted) {
        return std::unexpected{admitted.error()};
    }
    auto const frames = static_cast<uint32_t>(packet.payload.size() / format_.bytes_per_frame());
    if (!admit_sequence(packet.header.sequence.get(), frames)) {
        return std::unexpected{DropReason::sequence};
    }
    return frames;
}

auto Receiver::check_payload(rtp::RtpPacket const& packet, ip::IPv4Address const& source) const noexcept
    -> std::optional<DropReason>
{
    if (packet.header.payload_type() != payload_type_) {
        return DropReason::payload_type;
    }
    if (packet.payload.empty() || packet.payload.size() % format_.bytes_per_frame() != 0) {
        return DropReason::size;
    }
    if (source_filter_ && source != *source_filter_) {
        return DropReason::source;
    }
    return std::nullopt;
}

auto Receiver::admit_source(uint32_t const ssrc, uint64_t const now_index) noexcept -> std::expected<void, DropReason>
{
    if (ssrc_ && *ssrc_ != ssrc) {
        // Another source: refused while the learnt one is live, adopted once it
        // has been silent for a second (the sender restarted with a new SSRC).
        // Signed: after a PTP step backwards the learnt source still counts as live.
        if (static_cast<int64_t>(now_index - last_index_) < static_cast<int64_t>(format_.sample_rate)) {
            return std::unexpected{DropReason::ssrc};
        }
        reset_stream();
        if (sender_clock_) {
            offset_.store(0, std::memory_order_release);  // the new source's reports give its own
            return std::unexpected{DropReason::no_offset};
        }
    }
    if (!ssrc_) {
        ssrc_ = ssrc;
    }
    last_index_ = now_index;
    return {};
}

auto Receiver::admit_sequence(uint16_t const seq, uint32_t const frames) noexcept -> bool
{
    bool const was_validated = sequence_.validated();
    if (!sequence_.update(seq) && was_validated) {
        return false;  // an unconfirmed jump
    }
    // A packet on probation (a new source's first) is handed on all the same:
    // its timestamp, not its sequence number, places it, and index_of_rtp() puts it about now.
    return place(seq, frames);
}

void Receiver::note_arrival(uint32_t const timestamp, uint64_t const now_index) noexcept
{
    // RFC 3550 Appendix A.8 on the first packet of each drain (its index is the arrival time,
    // shared by the batch): transit = arrival - timestamp; J += (|D| - J) / 16, kept x16.
    uint32_t const transit = static_cast<uint32_t>(now_index) - timestamp;
    if (have_transit_ && now_index != last_arrival_index_) {
        auto const d = static_cast<int32_t>(transit - last_transit_);
        uint32_t const magnitude = d < 0 ? 0U - static_cast<uint32_t>(d) : static_cast<uint32_t>(d);
        jitter_x16_ += magnitude - ((jitter_x16_ + 8) >> 4U);
    }
    if (!have_transit_ || now_index != last_arrival_index_) {
        last_transit_ = transit;
        last_arrival_index_ = now_index;
        have_transit_ = true;
    }
    last_rtp_ts_ = timestamp;
    this_drain_.heard = true;
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
    ++counters_.packets;
    counters_.frames += frames;
    state_ = State::running;
}

auto Receiver::place(uint16_t const seq, uint32_t const frames) noexcept -> bool
{
    auto const ahead = static_cast<uint16_t>(seq - max_seq_);
    auto const behind = static_cast<uint16_t>(max_seq_ - seq);
    if (have_max_ && ahead != 0 && ahead < rtp::RtpSequence::MAX_DROPOUT) {
        advance_window(seq, ahead, frames);
        return true;
    }
    if (have_max_ && behind < WINDOW) {  // up to RtpSequence::MAX_MISORDER behind, as it accepts
        return mark_late(behind, frames);
    }
    start_window(seq);  // the first packet, or a jump RtpSequence confirmed: the sender restarted its sequence
    return true;
}

void Receiver::start_window(uint16_t const seq) noexcept
{
    have_max_ = true;
    max_seq_ = seq;
    window_.reset();
    window_.set(0);
}

void Receiver::advance_window(uint16_t const seq, uint16_t const ahead, uint32_t const frames) noexcept
{
    counters_.lost += uint64_t{ahead - 1U} * frames;  // assumes the stream's packets are all this size
    if (ahead >= WINDOW) {
        window_.reset();
    } else {
        window_ <<= ahead;
    }
    window_.set(0);
    max_seq_ = seq;
}

auto Receiver::mark_late(uint16_t const behind, uint32_t const frames) noexcept -> bool
{
    if (window_.test(behind)) {
        return false;  // a duplicate
    }
    window_.set(behind);
    counters_.late += frames;
    counters_.lost -= std::min<uint64_t>(counters_.lost, frames);
    return true;
}

}  // namespace statusbar::aes67
