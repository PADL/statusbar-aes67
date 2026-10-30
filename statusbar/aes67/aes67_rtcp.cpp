// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_rtcp.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/rtp/rtcp_ipmx.hpp"
#include "statusbar/rtp/rtcp_time.hpp"
#include "statusbar/rtp/rtp_error.hpp"

#include <algorithm>
#include <cmath>
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

/// The options a session takes: a CNAME of at most 255 bytes (RFC 3550 Section 6.5) and, for a
/// sender, a report interval.
[[nodiscard]] auto check_options(RtcpSessionOptions const& options, bool const sender) noexcept -> Status
{
    if (options.cname.size() > 255) {
        return failure(rtp::RtpError::field_too_long);
    }
    if (sender && options.interval_ms == 0) {
        return failure(std::errc::invalid_argument);
    }
    return success();
}

/// The `ts-refclk` value naming the grandmaster (RFC 7273), empty when there is none.
[[nodiscard]] auto ts_refclk_of(std::optional<Grandmaster> const& grandmaster) -> std::string
{
    return grandmaster ? sdp::ts_refclk_value(grandmaster->reference()) : std::string{};
}

/// The PCM Media Info Block for `format` (TR-10-3); `measured_sample_rate` is the nominal rate in PTP time.
[[nodiscard]] auto pcm_media_info(
    StreamFormat const& format, std::string_view const channel_order, uint32_t const measured_sample_rate) noexcept
    -> rtp::IpmxPcmMediaInfo
{
    return rtp::IpmxPcmMediaInfo{
        .sampling_rate = format.sample_rate,
        .sample_size = static_cast<uint8_t>(sdp::bytes_per_sample(format.encoding) * 8),
        .channel_count = static_cast<uint8_t>(std::min<uint16_t>(format.channels, 255)),
        .packet_time_us = static_cast<uint16_t>(format.packet_time_us()),
        .measured_sample_rate = measured_sample_rate,
        .channel_order = channel_order,
    };
}

}  // namespace

auto sender_clock_offset(SenderReportTime const& report, mediaclock::Timebase const& timebase, uint32_t const sample_rate) noexcept
    -> std::optional<uint32_t>
{
    auto const clock = mediaclock::PtpMediaClock::make(sample_rate);
    int64_t const domain_ns = timebase.to_domain(static_cast<int64_t>(report.ptp_ns));
    if (!clock || domain_ns < 0) {
        return std::nullopt;
    }
    uint64_t const half_sample_ns = mediaclock::PtpMediaClock::NS_PER_SECOND / (2 * uint64_t{sample_rate});
    uint64_t const index = clock->index_at(static_cast<uint64_t>(domain_ns) + half_sample_ns);
    return report.rtp_timestamp - static_cast<uint32_t>(index);
}

auto RtcpSession::open_sender(Sender const& sender, Transport& transport, RtcpSessionOptions options)
    -> StatusValue<std::unique_ptr<RtcpSession>>
{
    auto const rtcp = sender.rtcp_options();
    if (!rtcp) {
        return failure(Aes67Error::not_configured);
    }
    if (auto const checked = check_options(options, true); !checked) {
        return forward_failure(checked);
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
    if (auto const checked = check_options(options, false); !checked) {
        return forward_failure(checked);
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

void RtcpSession::reset_stats() noexcept
{  // NOLINT(statusbar-block-statements): the counts of the record, zeroed together
    stats_.sr_sent = 0;
    stats_.rr_sent = 0;
    stats_.bye_sent = 0;
    stats_.sr_received = 0;
    stats_.rr_received = 0;
    stats_.bye_received = 0;
    stats_.bad_packets = 0;
    stats_.send_errors = 0;
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
    note_sending();
    if (!stopped_bye_) {
        (void)send_bye(ssrc());
    }
    port_.reset();
}

void RtcpSession::note_sending() noexcept
{
    if (role_ != Role::sender) {
        return;
    }
    // The SSRC last seen sending is gone (stopped, or a new run under a new SSRC): its BYE.
    uint32_t const sending = sender_->send_state().ssrc;
    if (sending_ssrc_ != 0 && sending != sending_ssrc_) {
        bool const sent = send_bye(sending_ssrc_);
        stats_.bye_sent += sent ? 1 : 0;
        stopped_bye_ = sent && sending == 0;  // still sending (a new SSRC): close() owes its BYE
    } else if (sending != 0) {
        stopped_bye_ = false;
    }
    sending_ssrc_ = sending;
}

auto RtcpSession::send_bye(uint32_t const ssrc) noexcept -> bool
{
    // RFC 3550 Section 6.1: a compound packet starts with a report; an empty RR needs no clock.
    rtp::RtcpWriter writer{packet_};
    writer.receiver_report(ssrc).sdes(ssrc, options_.cname).bye(ssrc);
    if (!writer || !port_->send(writer.bytes())) {
        ++stats_.send_errors;
        return false;
    }
    return true;
}

void RtcpSession::tick(uint64_t const now_ns) noexcept
{
    if (!port_) {
        return;
    }
    if (port_->fd() < 0) {
        on_ready(now_ns);
    }
    note_sending();
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
    auto const state = sending_state();
    auto const at = report_time(now_ns);
    if (!state || !at) {
        return Report::none;
    }
    write_sender_report(writer, *state, *at, now_ns);
    return Report::sr;
}

auto RtcpSession::sending_state() const noexcept -> std::optional<SendState>
{
    SendState const state = sender_->send_state();
    if (!sender_->configured() || state.ssrc != sender_->ssrc()) {
        return std::nullopt;  // not sending, or not since the last configure()
    }
    return state;
}

auto RtcpSession::report_time(uint64_t const now_ns) const noexcept -> std::optional<ReportTime>
{
    // The SR pairs a sample index with its presentation time exactly (TR-10-1 Section 8.7: the
    // PTP truncated form), the index the latest at or before now, in the sender's domain.
    auto const clock = mediaclock::PtpMediaClock::make(sample_rate_);
    if (!clock) {
        return std::nullopt;
    }
    if (timebase_ == nullptr) {
        uint64_t const index = clock->index_at(now_ns);
        return ReportTime{.index = index, .time_ns = clock->time_of(index), .measured_rate = sample_rate_};
    }
    return domain_report_time(*clock, now_ns);
}

auto RtcpSession::domain_report_time(mediaclock::PtpMediaClock const& clock, uint64_t const now_ns) const noexcept
    -> std::optional<ReportTime>
{
    mediaclock::Timebase const timebase = timebase_->load();
    int64_t const domain_ns = timebase.to_domain(static_cast<int64_t>(now_ns));
    if (!timebase.running || domain_ns < 0) {
        return std::nullopt;  // the domain's outputs are not sending
    }
    uint64_t const index = clock.index_at(static_cast<uint64_t>(domain_ns));
    return ReportTime{
        .index = index,
        .time_ns = static_cast<uint64_t>(timebase.from_domain(static_cast<int64_t>(clock.time_of(index)))),
        .measured_rate = measured_rate(timebase.rate_offset),
    };
}

auto RtcpSession::measured_rate(double const rate_offset) const noexcept -> uint32_t
{
    // Kept until the rate moves a whole hertz from it, so fit noise does not churn the version.
    double const rate = static_cast<double>(sample_rate_) * (1.0 + rate_offset);
    return (info_block_rate_ != 0 && std::fabs(rate - info_block_rate_) < 1.0) ? info_block_rate_
                                                                               : static_cast<uint32_t>(std::llround(rate));
}

void RtcpSession::write_sender_report(rtp::RtcpWriter& writer, SendState const& state, ReportTime const& at, uint64_t const now_ns)
{
    auto const truncated = rtp::ptp_truncated_from_ns(at.time_ns);
    rtp::SenderInfo const info{
        .ntp_msw = truncated.seconds,
        .ntp_lsw = truncated.nanoseconds,
        .rtp_timestamp = mediaclock::PtpMediaClock::rtp_timestamp(at.index, 0),
        .packet_count = state.packets,
        .octet_count = state.octets,
    };
    refresh_info_block(at.measured_rate);
    writer.sender_report(state.ssrc, info, {}, info_block_);
    sent_[sent_next_] = {.lsr = rtp::lsr_of(truncated.seconds, truncated.nanoseconds), .sent_ns = now_ns};
    sent_next_ = (sent_next_ + 1) % SR_HISTORY;
}

void RtcpSession::refresh_info_block(uint32_t const measured_sample_rate)
{
    bool const same_inputs = info_block_version_ == sender_->info_block_version();
    if (same_inputs && info_block_rate_ == measured_sample_rate) {
        return;
    }
    if (same_inputs) {
        ++rate_changes_;  // TR-10-1 Section 8.7: a new version for any change of the content
    }
    info_block_version_ = sender_->info_block_version();
    info_block_rate_ = measured_sample_rate;
    rebuild_info_block(static_cast<uint8_t>(sender_->info_block_version() + rate_changes_), measured_sample_rate);
}

void RtcpSession::rebuild_info_block(uint8_t const version, uint32_t const measured_sample_rate)
{
    std::string const ts_refclk = ts_refclk_of(sender_->grandmaster());
    std::string const mediaclk = sdp::media_clock_value(sender_->media_clock());
    std::string const order = sender_->channel_order();
    rtp::IpmxInfoBlock const block{
        .version = version,
        .ts_refclk = ts_refclk,
        .mediaclk = mediaclk,
        .pcm = pcm_media_info(sender_->format(), order, measured_sample_rate),
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
    rtp::ReportBlock const block = report_block(state, now_ns);
    writer.receiver_report(own_ssrc_, std::span{&block, 1});
}

auto RtcpSession::report_block(ReceiveState const& state, uint64_t const now_ns) noexcept -> rtp::ReportBlock
{
    int64_t const cumulative = static_cast<int64_t>(state.expected) - static_cast<int64_t>(state.received);
    rtp::ReportBlock block{
        .ssrc = state.ssrc,
        .loss = {},
        .extended_highest_seq = state.extended_max,
        .jitter = state.jitter_x16 >> 4U,
        .lsr = {},
        .dlsr = {},
    };
    block.set_loss(take_fraction_lost(state), static_cast<int32_t>(std::clamp<int64_t>(cumulative, -0x80'0000, 0x7F'FFFF)));
    if (last_sr_ && last_sr_->ssrc == state.ssrc && now_ns >= last_sr_->arrival_ns) {
        block.lsr = last_sr_->lsr;
        block.dlsr = rtp::dlsr_from_ns(now_ns - last_sr_->arrival_ns);
    }
    return block;
}

auto RtcpSession::take_fraction_lost(ReceiveState const& state) noexcept -> uint8_t
{
    if (state.ssrc != reported_ssrc_ || state.expected < prior_.expected || state.received < prior_.received) {
        reported_ssrc_ = state.ssrc;  // a new source, or its sequence restarted
        prior_ = {};
    }
    // RFC 3550 Section 6.4.1 and Appendix A.3, from the receiver's cumulative figures.
    uint32_t const expected_interval = state.expected - prior_.expected;
    uint32_t const received_interval = state.received - prior_.received;
    prior_ = {.expected = state.expected, .received = state.received};
    int64_t const lost_interval = static_cast<int64_t>(expected_interval) - static_cast<int64_t>(received_interval);
    return (expected_interval == 0 || lost_interval <= 0) ? 0 : static_cast<uint8_t>((lost_interval << 8) / expected_interval);
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
    if (!compound || (!compound->sr && !compound->rr)) {
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
    if (auto const rtt = round_trip_ns(block, now_ns)) {
        stats_.rtt_ns = rtt;
    }
}

auto RtcpSession::round_trip_ns(rtp::ReportBlock const& block, uint64_t const now_ns) const noexcept -> std::optional<uint64_t>
{
    uint32_t const lsr = block.lsr.get();
    if (lsr == 0) {
        return std::nullopt;  // the receiver has heard no SR yet
    }
    for (auto const& sent : sent_) {
        if (sent.sent_ns != 0 && sent.lsr == lsr) {
            return rtp::rtt_ns(now_ns, sent.sent_ns, block.dlsr.get());
        }
    }
    return std::nullopt;
}

void RtcpSession::handle_as_receiver(rtp::RtcpCompound const& compound, uint64_t const now_ns) noexcept
{
    ReceiveState const state = receiver_->receive_state();
    // Before the stream's first packet any SR on its RTCP endpoint is the sender's.
    if (compound.sr && (!state.valid || compound.sr->ssrc == state.ssrc)) {
        take_stream_sr(*compound.sr, now_ns);
    }
    if (compound.rr) {
        ++stats_.rr_received;
    }
    uint32_t const remote = state.valid ? state.ssrc : stats_.remote_ssrc;
    if (remote != 0) {
        note_remote(compound, remote);
    }
}

void RtcpSession::take_stream_sr(rtp::RtcpSenderReport const& sr, uint64_t const now_ns) noexcept
{
    ++stats_.sr_received;
    last_sr_ =
        LastSenderReport{.lsr = rtp::lsr_of(sr.info.ntp_msw.get(), sr.info.ntp_lsw.get()), .ssrc = sr.ssrc, .arrival_ns = now_ns};
    stats_.remote_ssrc = sr.ssrc;
    stats_.remote_bye = false;
    take_sender_report(sr, now_ns);
}

void RtcpSession::note_remote(rtp::RtcpCompound const& compound, uint32_t const remote)
{
    if (compound.sdes && compound.sdes->ssrc == remote) {
        stats_.remote_cname.assign(compound.sdes->cname);
    }
    if (compound.bye && compound.bye->contains(remote)) {
        ++stats_.bye_received;
        stats_.remote_bye = true;
    }
}

void RtcpSession::take_sender_report(rtp::RtcpSenderReport const& sr, uint64_t const now_ns) noexcept
{
    // Only the IPMX Info Block says the time field is PTP time (TR-10-1 Section 8.7).
    if (!rtp::parse_ipmx_info_block(sr.extension)) {
        return;
    }
    auto const ptp_ns =
        rtp::ns_from_ptp_truncated({.seconds = sr.info.ntp_msw.get(), .nanoseconds = sr.info.ntp_lsw.get()}, now_ns);
    if (!ptp_ns) {
        return;
    }
    stats_.remote_sr = SenderReportTime{.rtp_timestamp = sr.info.rtp_timestamp.get(), .ptp_ns = *ptp_ns, .arrival_ns = now_ns};
    if (options_.on_sender_report) {
        options_.on_sender_report(*stats_.remote_sr);
    }
}

}  // namespace statusbar::aes67
