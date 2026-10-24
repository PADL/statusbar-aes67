// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_rtcp.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/rtp/rtcp_ipmx.hpp"
#include "statusbar/rtp/rtcp_time.hpp"
#include "statusbar/rtp/rtp_error.hpp"

#include <algorithm>
#include <random>
#include <utility>

namespace statusbar::aes67 {

namespace {

constexpr uint64_t NS_PER_MS = 1'000'000;

/// RFC 3550 Section 6.2: RTCP takes 5 % of the session bandwidth, here the stream's on the wire.
auto rtcp_bandwidth(StreamFormat const& format) noexcept -> double
{
    double const packets_per_second = static_cast<double>(format.sample_rate) / static_cast<double>(format.frames_per_packet);
    double const octets_per_packet = static_cast<double>(format.bytes_per_packet()) + 12.0 + 8.0 + 20.0;
    return 0.05 * packets_per_second * octets_per_packet;
}

auto default_cadence(StreamFormat const& format, uint64_t const minimum_ns, bool const sender) -> std::unique_ptr<RtcpCadence>
{
    // One sender and the receivers that report; SR + Info Block + SDES is about 200 octets with
    // the UDP and IP headers, an RR + SDES about 100.
    rtp::RtcpIntervalParams const params{
        .minimum_ns = minimum_ns,
        .members = 2,
        .senders = 1,
        .we_sent = sender,
        .rtcp_bandwidth = rtcp_bandwidth(format),
        .average_size = sender ? 200.0 : 100.0,
    };
    std::random_device random;
    return std::make_unique<Rfc3550Interval>(params, random());
}

}  // namespace

auto RtcpSession::open_sender(Sender const& sender, Transport& transport, RtcpSessionOptions options)
    -> StatusValue<std::unique_ptr<RtcpSession>>
{
    auto const rtcp = sender.rtcp_options();
    if (!rtcp) {
        return failure(Aes67Error::not_configured);
    }
    if (options.cname.size() > 255) {
        return failure(rtp::RtpError::field_too_long);
    }
    if (options.interval_ms == 0) {
        return failure(std::errc::invalid_argument);
    }
    auto port = transport.open_rtcp(*rtcp);
    if (!port) {
        return failure(port.error());
    }
    return std::unique_ptr<RtcpSession>{new RtcpSession{Role::sender, &sender, nullptr, std::move(*port), std::move(options)}};
}

auto RtcpSession::open_receiver(Receiver const& receiver, Transport& transport, RtcpSessionOptions options)
    -> StatusValue<std::unique_ptr<RtcpSession>>
{
    auto const rtcp = receiver.rtcp_options(options.dscp);
    if (!rtcp) {
        return failure(rtcp.error());
    }
    if (options.cname.size() > 255) {
        return failure(rtp::RtpError::field_too_long);
    }
    auto port = transport.open_rtcp(*rtcp);
    if (!port) {
        return failure(port.error());
    }
    return std::unique_ptr<RtcpSession>{new RtcpSession{Role::receiver, nullptr, &receiver, std::move(*port), std::move(options)}};
}

RtcpSession::RtcpSession(
    Role const role,
    Sender const* const sender,
    Receiver const* const receiver,
    std::unique_ptr<RtcpPort> port,
    RtcpSessionOptions options)
    : role_{role}
    , sender_{sender}
    , receiver_{receiver}
    , port_{std::move(port)}
    , options_{std::move(options)}
{
    StreamFormat const& format = sender_ != nullptr ? sender_->format() : receiver_->format();
    sample_rate_ = format.sample_rate;
    minimum_ns_ = (role_ == Role::sender ? uint64_t{options_.interval_ms} : uint64_t{RECEIVER_INTERVAL_MS}) * NS_PER_MS;
    if (!options_.cadence) {
        options_.cadence = default_cadence(format, minimum_ns_, role_ == Role::sender);
    }
    if (role_ == Role::receiver) {
        std::random_device random;
        own_ssrc_ = random();
    }
}

RtcpSession::~RtcpSession()
{
    close();
}

auto RtcpSession::ssrc() const noexcept -> uint32_t
{
    return role_ == Role::sender ? sender_->ssrc() : own_ssrc_;
}

auto RtcpSession::fd() const noexcept -> int
{
    return port_ ? port_->fd() : -1;
}

auto RtcpSession::dropped() const noexcept -> uint64_t
{
    return port_ ? port_->dropped() : 0;
}

void RtcpSession::close() noexcept
{
    if (!port_) {
        return;
    }
    // RFC 3550 Section 6.1: a compound packet starts with a report; an empty RR needs no clock.
    rtp::RtcpWriter writer{packet_};
    writer.receiver_report(ssrc()).sdes(ssrc(), options_.cname).bye(ssrc());
    if (!writer || !port_->send(writer.bytes())) {
        ++stats_.send_errors;
    }
    port_.reset();
}

void RtcpSession::tick(uint64_t const now_ns) noexcept
{
    if (!port_) {
        return;
    }
    if (port_->fd() < 0) {
        on_ready(now_ns);
    }
    if (next_report_ns_ == 0) {
        next_report_ns_ = now_ns + options_.cadence->next_interval_ns(true);
        return;
    }
    if (next_report_ns_ > now_ns && next_report_ns_ - now_ns > 4 * minimum_ns_) {
        next_report_ns_ = now_ns + options_.cadence->next_interval_ns(false);  // PTP stepped back
    }
    if (now_ns >= next_report_ns_) {
        send_report(now_ns);
        next_report_ns_ = now_ns + options_.cadence->next_interval_ns(false);
    }
}

void RtcpSession::send_report(uint64_t const now_ns) noexcept
{
    rtp::RtcpWriter writer{packet_};
    Report const report = write_report(writer, now_ns);
    if (report == Report::none) {
        return;
    }
    writer.sdes(ssrc(), options_.cname);
    if (!writer || !port_->send(writer.bytes())) {
        ++stats_.send_errors;
        return;
    }
    ++(report == Report::sr ? stats_.sr_sent : stats_.rr_sent);
}

auto RtcpSession::write_report(rtp::RtcpWriter& writer, uint64_t const now_ns) noexcept -> Report
{
    if (role_ == Role::receiver) {
        write_receiver_report(writer, now_ns);
        return Report::rr;
    }
    SendState const state = sender_->send_state();
    if (!sender_->configured() || state.ssrc != sender_->ssrc()) {
        return Report::none;  // not sending, or not since the last configure()
    }
    // The SR pairs a sample index with its presentation time exactly (TR-10-1 Section 8.7: the
    // PTP truncated form), the index the latest at or before now.
    auto const clock = mediaclock::PtpMediaClock::make(sample_rate_);
    if (!clock) {
        return Report::none;
    }
    uint64_t const index = clock->index_at(now_ns);
    uint64_t const time_ns = clock->time_of(index);
    auto const truncated = rtp::ptp_truncated_from_ns(time_ns);
    rtp::SenderInfo info{};
    info.ntp_msw = truncated.seconds;
    info.ntp_lsw = truncated.nanoseconds;
    info.rtp_timestamp = mediaclock::PtpMediaClock::rtp_timestamp(index, 0);
    info.packet_count = state.packets;
    info.octet_count = state.octets;
    refresh_info_block();
    writer.sender_report(state.ssrc, info, {}, info_block_);
    sent_[sent_next_] = {.lsr = rtp::lsr_of(truncated.seconds, truncated.nanoseconds), .sent_ns = now_ns};
    sent_next_ = (sent_next_ + 1) % SR_HISTORY;
    return Report::sr;
}

void RtcpSession::refresh_info_block()
{
    if (info_block_version_ == sender_->info_block_version()) {
        return;
    }
    info_block_version_ = sender_->info_block_version();
    StreamFormat const& format = sender_->format();
    std::string ts_refclk;
    if (auto const& grandmaster = sender_->grandmaster()) {
        ts_refclk = sdp::ts_refclk_value(sdp::PtpReference{.grandmaster = grandmaster->identity, .domain = grandmaster->domain});
    }
    std::string const mediaclk = sdp::media_clock_value(sdp::MediaClock{.direct_offset = 0});
    std::string const order = sender_->channel_order();
    rtp::IpmxInfoBlock const block{
        .version = *info_block_version_,
        .ts_refclk = ts_refclk,
        .mediaclk = mediaclk,
        .pcm =
            rtp::IpmxPcmMediaInfo{
                .sampling_rate = format.sample_rate,
                .sample_size = static_cast<uint8_t>(sdp::bytes_per_sample(format.encoding) * 8),
                .channel_count = static_cast<uint8_t>(std::min<uint16_t>(format.channels, 255)),
                .packet_time_us = static_cast<uint16_t>(format.packet_time_us()),
                .measured_sample_rate = format.sample_rate,  // PTP locked: the nominal rate
                .channel_order = order,
            },
    };
    info_block_.assign(rtp::ipmx_info_block_size(block), 0);
    if (!rtp::serialize_ipmx_info_block(block, info_block_)) {
        info_block_.clear();
    }
}

void RtcpSession::write_receiver_report(rtp::RtcpWriter& writer, uint64_t const now_ns) noexcept
{
    ReceiveState const state = receiver_->receive_state();
    if (!state.valid) {
        writer.receiver_report(own_ssrc_);
        return;
    }
    if (state.ssrc != reported_ssrc_ || state.expected < prior_expected_ || state.received < prior_received_) {
        reported_ssrc_ = state.ssrc;  // a new source, or its sequence restarted
        prior_expected_ = 0;
        prior_received_ = 0;
    }
    // RFC 3550 Section 6.4.1 and Appendix A.3, from the receiver's cumulative figures.
    uint32_t const expected_interval = state.expected - prior_expected_;
    uint32_t const received_interval = state.received - prior_received_;
    prior_expected_ = state.expected;
    prior_received_ = state.received;
    int64_t const lost_interval = static_cast<int64_t>(expected_interval) - static_cast<int64_t>(received_interval);
    uint8_t const fraction =
        (expected_interval == 0 || lost_interval <= 0) ? 0 : static_cast<uint8_t>((lost_interval << 8) / expected_interval);
    int64_t const cumulative = static_cast<int64_t>(state.expected) - static_cast<int64_t>(state.received);

    rtp::ReportBlock block{};
    block.ssrc = state.ssrc;
    block.set_loss(fraction, static_cast<int32_t>(std::clamp<int64_t>(cumulative, -0x80'0000, 0x7F'FFFF)));
    block.extended_highest_seq = state.extended_max;
    block.jitter = state.jitter_x16 >> 4U;
    if (last_sr_lsr_ && last_sr_ssrc_ == state.ssrc && now_ns >= last_sr_arrival_ns_) {
        block.lsr = *last_sr_lsr_;
        block.dlsr = rtp::dlsr_from_ns(now_ns - last_sr_arrival_ns_);
    }
    writer.receiver_report(own_ssrc_, std::span{&block, 1});
}

void RtcpSession::on_ready(uint64_t const now_ns) noexcept
{
    if (!port_) {
        return;
    }
    std::array<Datagram, BATCH> batch{};
    for (size_t round = 0; round < 4; ++round) {
        size_t const n = port_->receive(batch);
        for (size_t i = 0; i < n; ++i) {
            handle(batch[i].payload, now_ns);
        }
        if (n < batch.size()) {
            break;
        }
    }
}

void RtcpSession::handle(std::span<uint8_t const> const datagram, uint64_t const now_ns) noexcept
{
    auto const compound = rtp::parse_rtcp(datagram);
    if (!compound) {
        ++stats_.bad_packets;
        return;
    }
    uint32_t const reporter = compound->sr ? compound->sr->ssrc : compound->rr->ssrc;
    if (reporter == ssrc()) {
        return;  // our own, looped back by the group
    }
    if (role_ == Role::sender) {
        handle_as_sender(*compound, reporter, now_ns);
    } else {
        handle_as_receiver(*compound, now_ns);
    }
}

void RtcpSession::handle_as_sender(rtp::RtcpCompound const& compound, uint32_t const reporter, uint64_t const now_ns) noexcept
{
    uint32_t const own = ssrc();
    if (compound.sr) {
        ++stats_.sr_received;
        if (auto const block = compound.sr->blocks.find(own)) {
            take_report_block(*block, now_ns);
            stats_.remote_ssrc = reporter;
        }
    }
    if (compound.rr) {
        ++stats_.rr_received;
        if (auto const block = compound.rr->blocks.find(own)) {
            take_report_block(*block, now_ns);
            stats_.remote_ssrc = reporter;
        }
    }
    if (compound.sdes && compound.sdes->ssrc == stats_.remote_ssrc && reporter == stats_.remote_ssrc) {
        stats_.remote_cname.assign(compound.sdes->cname);
    }
    if (compound.bye && stats_.remote_ssrc != 0 && compound.bye->contains(stats_.remote_ssrc)) {
        ++stats_.bye_received;
        stats_.remote_bye = true;
    }
}

void RtcpSession::take_report_block(rtp::ReportBlock const& block, uint64_t const now_ns) noexcept
{
    stats_.remote_fraction_lost = block.fraction_lost();
    stats_.remote_cumulative_lost = block.cumulative_lost();
    stats_.remote_jitter = block.jitter.get();
    stats_.remote_jitter_us = static_cast<uint32_t>((uint64_t{block.jitter.get()} * 1'000'000) / sample_rate_);
    stats_.remote_bye = false;
    uint32_t const lsr = block.lsr.get();
    if (lsr == 0) {
        return;  // the receiver has heard no SR yet
    }
    for (auto const& sent : sent_) {
        if (sent.sent_ns != 0 && sent.lsr == lsr) {
            stats_.rtt_ns = rtp::rtt_ns(now_ns, sent.sent_ns, block.dlsr.get());
            return;
        }
    }
}

void RtcpSession::handle_as_receiver(rtp::RtcpCompound const& compound, uint64_t const now_ns) noexcept
{
    ReceiveState const state = receiver_->receive_state();
    if (compound.sr) {
        // Before the stream's first packet any SR on its RTCP endpoint is the sender's.
        if (!state.valid || compound.sr->ssrc == state.ssrc) {
            ++stats_.sr_received;
            last_sr_lsr_ = rtp::lsr_of(compound.sr->info.ntp_msw.get(), compound.sr->info.ntp_lsw.get());
            last_sr_ssrc_ = compound.sr->ssrc;
            last_sr_arrival_ns_ = now_ns;
            stats_.remote_ssrc = compound.sr->ssrc;
            stats_.remote_bye = false;
        }
    }
    if (compound.rr) {
        ++stats_.rr_received;
    }
    uint32_t const remote = state.valid ? state.ssrc : stats_.remote_ssrc;
    if (remote == 0) {
        return;
    }
    if (compound.sdes && compound.sdes->ssrc == remote) {
        stats_.remote_cname.assign(compound.sdes->cname);
    }
    if (compound.bye && compound.bye->contains(remote)) {
        ++stats_.bye_received;
        stats_.remote_bye = true;
    }
}

}  // namespace statusbar::aes67
