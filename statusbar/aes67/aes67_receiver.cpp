// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_receiver.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/rtp/rtp_header.hpp"

#include <algorithm>
#include <array>

namespace statusbar::aes67 {

namespace {

/// Sequence numbers the reorder window remembers: as far back as RtpSequence
/// accepts a late packet.
constexpr size_t WINDOW = rtp::RtpSequence::MAX_MISORDER + 1;
static_assert(WINDOW <= 128);

}  // namespace

Receiver::Receiver(StreamFormat const& format, FrameHandler handler, Transport& transport) noexcept
    : format_{format}
    , handler_{std::move(handler)}
    , transport_{transport}
{}

auto Receiver::configure(sdp::Session const& session, size_t const media_index) -> Status
{
    if (media_index >= session.media.size()) {
        return failure(Aes67Error::no_media);
    }
    auto media = session.media[media_index];
    (void)media.prefer(format_.encoding);  // else the primary, refused below
    auto const connection = session.effective_connection(media);
    if (!connection) {
        return failure(Aes67Error::no_connection);
    }
    auto const clock = session.effective_media_clock(media);
    auto const kind = clock ? clock->kind() : sdp::MediaClockKind::other;
    if (sender_clock_ && kind != sdp::MediaClockKind::sender) {
        return failure(Aes67Error::not_sender_clocked);
    }
    if (!sender_clock_ && kind != sdp::MediaClockKind::direct) {
        return failure(Aes67Error::not_ptp_locked);
    }
    if (!mediaclock::PtpMediaClock::supports(media.sample_rate)) {
        return failure(Aes67Error::unsupported_format);
    }
    if (media.encoding != format_.encoding) {
        return failure(Aes67Error::encoding_mismatch);
    }
    if (media.channels != format_.channels || media.sample_rate != format_.sample_rate) {
        return failure(Aes67Error::format_mismatch);
    }

    clear();
    auto port = transport_.open_receive({.address = connection->address, .port = media.port});
    if (!port) {
        state_ = State::error;
        error_ = port.error();
        return failure(port.error());
    }
    port_ = std::move(*port);
    payload_type_ = media.payload_type;
    offset_.store(sender_clock_ ? 0 : OFFSET_SET | *clock->direct_offset, std::memory_order_release);
    auto const filter = session.effective_source_filter(media);
    if (filter && (!filter->destination || *filter->destination == connection->address)) {
        source_filter_ = filter->source;
    }
    set_rtcp(session, media, *connection);
    receive_state_.store({});  // not drained while configured, so drain() is not storing too
    state_ = State::ready;
    error_.clear();
    return success();
}

void Receiver::set_rtcp(sdp::Session const& session, sdp::Media const& media, sdp::Connection const& connection)
{
    rtcp_.reset();
    rtcp_error_.clear();
    if (media.rtcp_mux) {
        rtcp_error_ = make_error_code(Aes67Error::rtcp_multiplexed);
        return;
    }
    Endpoint const endpoint = media.rtcp
        ? Endpoint{.address = media.rtcp->address.value_or(connection.address), .port = media.rtcp->port}
        : rtcp_endpoint_of({.address = connection.address, .port = media.port});
    RtcpOptions options{
        .destination = endpoint,
        .local = {},
        .ttl = connection.ttl != 0 ? connection.ttl : sdp::Connection::DEFAULT_MULTICAST_TTL,
    };
    bool const unicast_stream = !connection.address.is_multicast();
    if (media.rtcp && media.rtcp->address && !endpoint.address.is_multicast()) {
        // RFC 3605: the address is where reports go; they arrive here at the same port.
        options.local = {.address = unicast_stream ? connection.address : ip::IPv4Address{}, .port = endpoint.port};
    } else if (!endpoint.address.is_multicast()) {
        // A unicast stream: reports arrive at this address and go to the sender, at the same port.
        auto const peer = source_filter_ ? source_filter_ : ip::parse_ipv4(session.origin_address);
        if (!peer) {
            rtcp_error_ = make_error_code(Aes67Error::no_rtcp_peer);
            return;
        }
        options.local = endpoint;
        options.destination = {.address = *peer, .port = endpoint.port};
    }
    rtcp_ = options;
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
{
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
    newest_end_ = 0;
    newest_frames_ = 0;
    heard_ = false;
    uint64_t offset = offset_.load(std::memory_order_acquire);
    std::array<Datagram, BATCH> batch{};
    bool backlog = true;  // every batch came back full: more may be waiting
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
            backlog = false;
            break;
        }
    }
    if (heard_ && ssrc_) {
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
    if (backlog) {
        next_poll_index_ = 0;  // the rest next tick
    } else if (newest_frames_ != 0) {
        // The next packet, [end, end + n), is sent no earlier than end + n:
        // ask again a tick before that, but never later than a packet time
        // and a tick from now.
        uint64_t const tick = (format_.sample_rate + 7999) / 8000;
        uint64_t const due = newest_end_ + newest_frames_ - std::min<uint64_t>(tick, newest_end_ + newest_frames_);
        next_poll_index_ = std::min(due, now_index + newest_frames_ + tick);
        gate_set_at_ = now_index;
    }
    // (Nothing came: the gate stays open, every tick until a packet does.)
}

void Receiver::accept(Datagram const& datagram, uint64_t const now_index, uint64_t const offset) noexcept
{
    auto const packet = rtp::parse_rtp(datagram.payload);
    uint32_t const frame_bytes = format_.bytes_per_frame();
    if (!packet || packet->header.payload_type() != payload_type_ || packet->payload.empty() ||
        packet->payload.size() % frame_bytes != 0 || (source_filter_ && datagram.source.address != *source_filter_)) {
        ++counters_.dropped;
        return;
    }
    uint32_t const ssrc = packet->header.ssrc.get();
    if (ssrc_ && *ssrc_ != ssrc) {
        // Another source: refused while the learnt one is live, adopted once it
        // has been silent for a second (the sender restarted with a new SSRC).
        // Signed: after a PTP step backwards the learnt source still counts as live.
        if (static_cast<int64_t>(now_index - last_index_) < static_cast<int64_t>(format_.sample_rate)) {
            ++counters_.dropped;
            return;
        }
        reset_stream();
        if (sender_clock_) {
            offset_.store(0, std::memory_order_release);  // the new source's reports give its own
            ++counters_.dropped;
            return;
        }
    }
    if (!ssrc_) {
        ssrc_ = ssrc;
    }
    last_index_ = now_index;

    uint16_t const seq = packet->header.sequence.get();
    bool const was_validated = sequence_.validated();
    if (!sequence_.update(seq) && was_validated) {
        ++counters_.dropped;  // an unconfirmed jump
        return;
    }
    // A packet on probation (a new source's first) is handed on all the same:
    // its timestamp, not its sequence number, places it, and index_of_rtp()
    // puts it about now.
    auto const frames = static_cast<uint32_t>(packet->payload.size() / frame_bytes);
    if (!place(seq, frames)) {
        ++counters_.dropped;
        return;
    }

    // RFC 3550 Appendix A.8 on the first packet of each drain (its index is the arrival time,
    // shared by the batch): transit = arrival - timestamp; J += (|D| - J) / 16, kept x16.
    uint32_t const timestamp = packet->header.timestamp.get();
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
    heard_ = true;
    if ((offset & OFFSET_SET) == 0) {
        ++counters_.dropped;  // learnt, but not placed before the sender's first report
        return;
    }

    uint64_t const index = mediaclock::PtpMediaClock::index_of_rtp(timestamp, static_cast<uint32_t>(offset), now_index);
    if (index + frames > newest_end_) {
        newest_end_ = index + frames;
        newest_frames_ = frames;
    }
    if (handler_) {
        handler_(index, frames, packet->payload);
    }
    ++counters_.packets;
    counters_.frames += frames;
    state_ = State::running;
}

auto Receiver::place(uint16_t const seq, uint32_t const frames) noexcept -> bool
{
    auto restart = [&] {
        max_seq_ = seq;
        window_.reset();
        window_.set(0);
    };
    if (!have_max_) {
        have_max_ = true;
        restart();
        return true;
    }
    auto const ahead = static_cast<uint16_t>(seq - max_seq_);
    auto const behind = static_cast<uint16_t>(max_seq_ - seq);
    if (ahead != 0 && ahead < rtp::RtpSequence::MAX_DROPOUT) {
        counters_.lost += uint64_t{ahead - 1U} * frames;  // assumes the stream's packets are all this size
        if (ahead >= WINDOW) {
            window_.reset();
        } else {
            window_ <<= ahead;
        }
        window_.set(0);
        max_seq_ = seq;
        return true;
    }
    if (behind < WINDOW) {  // up to RtpSequence::MAX_MISORDER behind, as it accepts
        if (window_.test(behind)) {
            return false;  // a duplicate
        }
        window_.set(behind);
        counters_.late += frames;
        counters_.lost -= std::min<uint64_t>(counters_.lost, frames);
        return true;
    }
    // A jump RtpSequence confirmed: the sender restarted its sequence.
    restart();
    return true;
}

}  // namespace statusbar::aes67
