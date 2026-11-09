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

/// RFC 3550 Section 6.2: RTCP uses 5 % of the session bandwidth, here the stream's on the wire.
auto rtcp_bandwidth(StreamFormat const& format) noexcept -> double
{
    double const packets_per_second = static_cast<double>(format.sample_rate) / static_cast<double>(format.frames_per_packet);
    double const octets_per_packet = static_cast<double>(format.bytes_per_packet()) + 12.0 + 8.0 + 20.0;
    return 0.05 * packets_per_second * octets_per_packet;
}

/// RFC 3550 Section 6.3.1's cadence: one sender and the receivers that report; SR + Info Block + SDES is about 200
/// octets with the UDP and IP headers, an RR + SDES about 100.
auto default_cadence(StreamFormat const& format, uint64_t const minimum_ns, bool const sender) -> std::unique_ptr<RtcpCadence>
{
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

/// The options a session accepts: a CNAME of at most 255 bytes (RFC 3550 Section 6.5).
[[nodiscard]] auto check_options(RtcpSessionOptions const& options) noexcept -> Status
{
    return options.cname.size() > 255 ? failure(rtp::RtpError::field_too_long) : success();
}

/// The Info Block's `ts-refclk` (VSF TR-10-1 Section 8.7): the SDP's first value, empty when it has none.
[[nodiscard]] auto ts_refclk_of(sdp::ReferenceClocks const& clocks) -> std::string
{
    if (!clocks.ptp.empty()) {
        return sdp::ts_refclk_value(clocks.ptp.front());
    }
    return clocks.other.empty() ? std::string{} : clocks.other.front();
}

/// SR spacing in frames: int(10 ms / ptime) packets, at least one (VSF TR-10-1 Section 8.10.1).
[[nodiscard]] constexpr auto report_stride(StreamFormat const& format) noexcept -> uint64_t
{
    uint64_t const fpp = std::max<uint64_t>(format.frames_per_packet, 1);
    uint64_t const spacing = (uint64_t{format.sample_rate} * RtcpSession::REPORT_SPACING_US) / 1'000'000;
    return std::max<uint64_t>(spacing / fpp, 1) * fpp;
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

auto RtcpSession::open_sender(Sender& sender, Transport& transport, RtcpSessionOptions options)
    -> StatusValue<std::unique_ptr<RtcpSession>>
{
    auto const rtcp = sender.rtcp_options();
    if (!rtcp) {
        return failure(Aes67Error::not_configured);
    }
    if (auto const checked = check_options(options); !checked) {
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
    if (auto const checked = check_options(options); !checked) {
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
    Sender* const sender,
    Receiver const* const receiver,
    std::unique_ptr<RtcpPort> port,
    RtcpSessionOptions options)
    : role_{role}
    , sender_{sender}
    , receiver_{receiver}
    , port_{std::move(port)}
    , options_{std::move(options)}
    , sample_rate_{format_of(sender, receiver).sample_rate}
    , minimum_ns_{uint64_t{role == Role::sender ? SENDER_INTERVAL_MS : RECEIVER_INTERVAL_MS} * NS_PER_MS}
    , report_stride_{report_stride(format_of(sender, receiver))}
{
    if (ipmx()) {
        hold_ = sender_->hold_reports();
    }
    if (!ipmx() && !options_.cadence) {
        options_.cadence = default_cadence(format_of(sender, receiver), minimum_ns_, role_ == Role::sender);
    }
    std::random_device random;
    own_ssrc_ = role_ == Role::receiver ? random() : 0;
}

RtcpSession::~RtcpSession()
{
    close();
}

auto RtcpSession::format_of(Sender const* const sender, Receiver const* const receiver) noexcept -> StreamFormat const&
{
    return sender != nullptr ? sender->format() : receiver->format();
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
    hold_ = {};  // an ipmx sender's next run waits for no SR
}

void RtcpSession::note_sending() noexcept
{
    if (role_ != Role::sender) {
        return;
    }
    // The SSRC last seen sending is gone (stopped, or a new run under a new SSRC): its BYE. A run held for its first
    // SR has sent no packet: not sending.
    SendState const state = sender_->send_state();
    uint32_t const sending = state.next_index != state.first_index ? state.ssrc : 0;  // packets, which wrap, would not do
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
    if (ipmx()) {
        send_due_report(now_ns);
    } else {
        schedule_report(now_ns);
    }
}

void RtcpSession::schedule_report(uint64_t const now_ns) noexcept
{
    if (next_report_ns_ == 0) {
        next_report_ns_ = now_ns + options_.cadence->next_interval_ns(true);
        return;
    }
    if (next_report_ns_ > now_ns && next_report_ns_ - now_ns > 4 * minimum_ns_) {
        next_report_ns_ = now_ns + options_.cadence->next_interval_ns(false);  // PTP stepped back
    }
    if (now_ns >= next_report_ns_) {
        role_ == Role::receiver ? send_receiver_report(now_ns) : send_scheduled_sender_report(now_ns);
        next_report_ns_ = now_ns + options_.cadence->next_interval_ns(false);
    }
}

void RtcpSession::send_scheduled_sender_report(uint64_t const now_ns) noexcept
{
    auto const state = sending_state();
    bool const sending = state && state->next_index != state->first_index;  // a packet of this run has gone
    auto const at = sending ? report_time(state->next_index) : std::nullopt;
    if (at) {
        (void)send_sender_report(*state, *at, now_ns);
    }
}

void RtcpSession::send_receiver_report(uint64_t const now_ns) noexcept
{
    rtp::RtcpWriter writer{packet_};
    write_receiver_report(writer, now_ns);
    writer.sdes(own_ssrc_, options_.cname);
    if (!writer || !port_->send(writer.bytes())) {
        ++stats_.send_errors;
        return;
    }
    ++stats_.rr_sent;
}

void RtcpSession::send_due_report(uint64_t const now_ns) noexcept
{
    auto const state = sending_state();
    auto const index = state ? due_report(*state) : std::nullopt;
    auto const at = index ? report_time(*index) : std::nullopt;
    if (at && send_sender_report(*state, *at, now_ns)) {
        report_index_ = *index + report_stride_;
    }
}

auto RtcpSession::next_wake_ns(uint64_t const now_ns) const noexcept -> std::optional<uint64_t>
{
    if (!ipmx()) {
        return next_report_ns_ != 0 ? std::optional{next_report_ns_} : std::nullopt;
    }
    auto const state = sending_state();
    if (!state || state->ssrc != report_ssrc_ || report_index_ < report_stride_) {
        return std::nullopt;  // nothing sending, or a run not seen yet: the next tick finds it
    }
    return now_ns + (wake_frames(*state) * mediaclock::PtpMediaClock::NS_PER_SECOND / sample_rate_);
}

auto RtcpSession::wake_frames(SendState const& state) const noexcept -> uint64_t
{
    // Midway through the next SR's window, at the sender's rate: a wake serves every session whose window is open.
    // Past it (the SR could not go), when its packet leaves, or a window on: never now again.
    uint64_t const middle = report_index_ - (report_stride_ / 2);
    uint64_t const ahead = report_index_ > state.next_index ? report_index_ - state.next_index : report_stride_;
    return middle > state.next_index ? middle - state.next_index : ahead;
}

auto RtcpSession::due_report(SendState const& state) noexcept -> std::optional<uint64_t>
{
    if (state.ssrc != report_ssrc_ || state.first_index != report_first_) {  // a new run, or its held packet moved
        report_ssrc_ = state.ssrc;
        report_first_ = report_index_ = state.first_index;
    }
    if (state.next_index > report_index_) {  // its packet left without it: the next on the grid
        report_index_ += ((state.next_index - report_index_ + report_stride_ - 1) / report_stride_) * report_stride_;
    } else if (report_index_ - state.next_index > 2 * report_stride_) {  // the clock went back
        report_index_ = state.next_index + report_stride_;
    }
    // Due before its packet leaves and once the one before it on the grid has (the first: while it is held).
    bool const due = report_index_ == state.first_index || state.next_index + report_stride_ > report_index_;
    return due ? std::optional{report_index_} : std::nullopt;
}

auto RtcpSession::send_sender_report(SendState const& state, ReportTime const& at, uint64_t const now_ns) noexcept -> bool
{
    rtp::RtcpWriter writer{packet_};
    write_sender_report(writer, state, at, now_ns);
    writer.sdes(state.ssrc, options_.cname);
    if (!writer || !port_->send(writer.bytes())) {
        ++stats_.send_errors;
        return false;
    }
    ++stats_.sr_sent;
    return true;
}

auto RtcpSession::sending_state() const noexcept -> std::optional<SendState>
{
    SendState const state = sender_->send_state();
    if (!sender_->configured() || state.ssrc != sender_->ssrc()) {
        return std::nullopt;  // not sending, or not since the last configure()
    }
    return state;
}

auto RtcpSession::report_time(uint64_t const index) const noexcept -> std::optional<ReportTime>
{
    auto const clock = mediaclock::PtpMediaClock::make(sample_rate_);
    if (!clock) {
        return std::nullopt;
    }
    if (timebase_ == nullptr) {  // the index is the sender's clock's own: its time, unconverted
        return ReportTime{.index = index, .time_ns = clock->time_of(index), .measured_rate = sample_rate_};
    }
    return domain_report_time(clock->time_of(index), index);
}

auto RtcpSession::domain_report_time(uint64_t const domain_ns, uint64_t const index) const noexcept -> std::optional<ReportTime>
{
    mediaclock::Timebase const timebase = timebase_->load();
    int64_t const ptp_ns = timebase.from_domain(static_cast<int64_t>(domain_ns));
    if (!timebase.running || ptp_ns < 0) {
        return std::nullopt;  // the domain's outputs are not sending
    }
    return ReportTime{
        .index = index, .time_ns = static_cast<uint64_t>(ptp_ns), .measured_rate = measured_rate(timebase.rate_offset)};
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
    std::string const ts_refclk = ts_refclk_of(sender_->references());
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
    block.set_loss(fraction_lost(state), static_cast<int32_t>(std::clamp<int64_t>(cumulative, -0x80'0000, 0x7F'FFFF)));
    if (last_sr_ && last_sr_->ssrc == state.ssrc && now_ns >= last_sr_->arrival_ns) {
        block.lsr = last_sr_->lsr;
        block.dlsr = rtp::dlsr_from_ns(now_ns - last_sr_->arrival_ns);
    }
    return block;
}

auto RtcpSession::fraction_lost(ReceiveState const& state) noexcept -> uint8_t
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
            note_report_block(*block, now_ns);
            stats_.remote_ssrc = reporter;
        }
    }
    if (compound.rr) {
        ++stats_.rr_received;
        if (auto const block = compound.rr->blocks.find(own)) {
            note_report_block(*block, now_ns);
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

void RtcpSession::note_report_block(rtp::ReportBlock const& block, uint64_t const now_ns) noexcept
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
        note_stream_sr(*compound.sr, now_ns);
    }
    if (compound.rr) {
        ++stats_.rr_received;
    }
    uint32_t const remote = state.valid ? state.ssrc : stats_.remote_ssrc;
    if (remote != 0) {
        note_remote(compound, remote);
    }
}

void RtcpSession::note_stream_sr(rtp::RtcpSenderReport const& sr, uint64_t const now_ns) noexcept
{
    ++stats_.sr_received;
    last_sr_ =
        LastSenderReport{.lsr = rtp::lsr_of(sr.info.ntp_msw.get(), sr.info.ntp_lsw.get()), .ssrc = sr.ssrc, .arrival_ns = now_ns};
    stats_.remote_ssrc = sr.ssrc;
    stats_.remote_bye = false;
    note_sender_report(sr, now_ns);
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

void RtcpSession::note_sender_report(rtp::RtcpSenderReport const& sr, uint64_t const now_ns) noexcept
{
    // The Info Block says the time field is the sender's Internal Clock, PTP truncated (VSF TR-10-1 Section 8.7);
    // whether that clock is PTP's is its ts-refclk's to say, which the handler checks.
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
