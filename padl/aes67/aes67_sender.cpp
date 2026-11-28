// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/aes67/aes67_sender.hpp"

#include "padl/aes67/aes67_error.hpp"
#include "padl/mediaclock/mediaclock_ptp_clock.hpp"
#include "padl/rtp/rtp_header.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ip/ip_ipv4_address_format.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace padl::aes67 {

namespace {

/// Fails unsupported_format unless the media clock supports the rate and a packet holds frames,
/// packet_too_large when a packet would not fit.
[[nodiscard]] auto check_send_format(StreamFormat const& format) noexcept -> statusbar::Status
{
    if (!mediaclock::PtpMediaClock::supports(format.sample_rate) || format.frames_per_packet == 0 || format.channels == 0) {
        return statusbar::failure(Aes67Error::unsupported_format);
    }
    if (format.bytes_per_packet() > Sender::MAX_PAYLOAD) {
        return statusbar::failure(Aes67Error::packet_too_large);
    }
    return statusbar::success();
}

}  // namespace

Sender::Sender(StreamFormat const& format, FrameProvider provider, Transport& transport, std::string name)
    : format_{format}
    , provider_{std::move(provider)}
    , transport_{transport}
    , name_{std::move(name)}
    , schedule_{format}
    , send_offset_{default_send_offset(format)}
{}

auto Sender::configure(Destination const& destination) -> statusbar::Status
{
    if (auto const checked = check_send_format(format_); !checked) {
        return checked;
    }
    if (auto const opened = open_port(destination); !opened) {
        return opened;
    }
    seed_run();
    renew_session();
    become_ready();
    return statusbar::success();
}

auto Sender::configure(sdp::Session const& session) -> statusbar::Status
{
    auto const media = select_media(session, 0, format_.encoding);
    if (!media) {
        return statusbar::forward_failure(media);
    }
    auto const connection = session.effective_connection(*media);
    if (!connection) {
        return statusbar::failure(Aes67Error::no_connection);
    }
    if (auto const matched = check_media_format(*media, format_); !matched) {
        return matched;
    }
    return configure(Destination{
        .address = connection->address,
        .port = media->port,
        .payload_type = media->payload_type,
        .ttl = connection->ttl != 0 ? connection->ttl : sdp::Connection::DEFAULT_MULTICAST_TTL,
    });
}

auto Sender::open_port(Destination const& destination) -> statusbar::Status
{
    clear();
    auto port = transport_.open_send_stream(send_plan(destination));
    if (!port) {
        state_ = State::error;
        error_ = port.error();
        return statusbar::failure(port.error());
    }
    adopt_port(std::move(*port));
    destination_ = destination;
    return statusbar::success();
}

void Sender::adopt_port(std::unique_ptr<SendPort> port) noexcept
{
    port_ = std::move(port);
    frames_ = port_->frames();
    port_send_offset_ = UINT32_MAX;  // told at its first tick
    schedule_.set_start_lead(frames_ != nullptr ? frames_->start_lead_frames() : 0);
}

auto Sender::send_plan(Destination const& destination) const noexcept -> SendPlan
{
    return SendPlan{
        .destination = {.address = destination.address, .port = destination.port},
        .ttl = destination.ttl,
        .dscp = dscp_,
        .vlan = vlan_,
        .format = format_,
        .payload_type = destination.payload_type,
        .phase = schedule_.phase(),
        .grandmaster = grandmaster_,
    };
}

auto Sender::set_vlan(std::optional<statusbar::ieee::VlanTag> const& vlan) noexcept -> statusbar::Status
{
    if (port_ != nullptr) {
        if (auto const set = port_->set_vlan(vlan); !set) {
            return set;
        }
    }
    vlan_ = vlan;
    return statusbar::success();
}

void Sender::seed_run()
{
    payload_.assign(format_.bytes_per_packet(), 0);
    schedule_.seed();
    rtcp_counts_ = {};
}

void Sender::renew_session()
{
    if (session_id_ == 0) {
        session_id_ = stable_session_id(std::format("{}/{}", statusbar::ip::to_string(transport_.local_address()), name_));
    }
    bump_session_version();
    ++info_block_version_;
}

void Sender::become_ready() noexcept
{                           // NOLINT(statusbar-block-statements): the run's state, reset together
    send_state_.store({});  // not ticked while configured, so tick() is not storing too
    paused_ = false;
    schedule_.reset();
    state_ = State::ready;
    error_.clear();
}

void Sender::clear() noexcept
{  // NOLINT(statusbar-block-statements): the port and its run, reset together
    port_.reset();
    frames_ = nullptr;
    gathered_.store(false, std::memory_order_relaxed);
    run_started_ = false;
    schedule_.stop();
    state_ = State::idle;
}

auto Sender::gather_from(std::span<FrameRoute const> const routes) -> statusbar::Status
{
    if (port_ == nullptr) {
        return routes.empty() ? statusbar::success() : statusbar::failure(std::errc::not_connected);
    }
    if (frames_ != nullptr) {
        return frames_->gather_from(routes);
    }
    if (!routes.empty()) {
        // Its lost and resends read at each tick from here, a handover still under way (a failure here) included.
        gathered_.store(true, std::memory_order_relaxed);
    }
    return port_->gather_from(routes);
}

void Sender::continue_from(Sender const& previous)
{  // NOLINT(statusbar-block-statements): the settings carried over, field by field
    send_offset_.store(previous.send_offset(), std::memory_order_relaxed);
    grandmaster_ = previous.grandmaster_;
    sender_clock_ = previous.sender_clock_;
    name_ = previous.name_;
    attributes_ = previous.attributes_;
    session_id_ = previous.session_id_;
    session_version_ = previous.session_version_;
    dscp_ = previous.dscp_;
    channel_order_ = previous.channel_order_;
    st2110_ = previous.st2110_;
    info_block_version_ = previous.info_block_version_;
}

void Sender::bump_session_version()
{
    if (session_version_ == 0) {
        // Seconds since the epoch: a restart's versions follow the last run's.
        auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
        session_version_ = static_cast<uint64_t>(std::max<int64_t>(seconds.count(), 0)) & SESSION_ID_MASK;
    }
    session_version_ = (session_version_ + 1) & SESSION_ID_MASK;
}

void Sender::set_name(std::string name)
{
    if (name == name_) {
        return;
    }
    name_ = std::move(name);
    bump_session_version();
}

void Sender::set_attributes(std::vector<sdp::Attribute> attributes)
{
    if (attributes == attributes_) {
        return;
    }
    attributes_ = std::move(attributes);
    bump_session_version();
}

void Sender::set_grandmaster(std::optional<Grandmaster> const& grandmaster)
{
    if (grandmaster == grandmaster_) {
        return;
    }
    // The Info Block names the grandmaster alone: a change of `traceable` only is not its change.
    bool const renamed = grandmaster.transform(&Grandmaster::reference) != grandmaster_.transform(&Grandmaster::reference);
    grandmaster_ = grandmaster;
    bump_session_version();  // the SDP changed: a receiver keying on o= sees a new version
    info_block_version_ += renamed ? 1 : 0;
}

auto Sender::references() const -> sdp::ReferenceClocks
{
    if (grandmaster_) {
        return grandmaster_->references();
    }
    sdp::ReferenceClocks clocks;
    if (auto const mac = transport_.local_mac()) {
        clocks.other.push_back(sdp::localmac_value(*mac));
    }
    return clocks;
}

Sender::ReportHold::ReportHold(Sender& sender) noexcept
    : sender_{&sender}
{}

Sender::ReportHold::~ReportHold()
{
    if (sender_ != nullptr) {
        sender_->count_hold(false);
    }
}

Sender::ReportHold::ReportHold(ReportHold&& other) noexcept
    : sender_{std::exchange(other.sender_, nullptr)}
{}

auto Sender::ReportHold::operator=(ReportHold&& other) noexcept -> ReportHold&
{
    if (this != &other) {
        ReportHold const ended{std::move(*this)};  // this one's hold ends here
        sender_ = std::exchange(other.sender_, nullptr);
    }
    return *this;
}

auto Sender::hold_reports() -> ReportHold
{
    count_hold(true);
    return ReportHold{*this};
}

void Sender::count_hold(bool const held)
{
    // A hold ends only once, after it began: the count never goes below 0.
    uint32_t const before = held ? holds_.fetch_add(1, std::memory_order_relaxed) : holds_.fetch_sub(1, std::memory_order_relaxed);
    if (before == (held ? 0U : 1U)) {
        bump_session_version();  // the first began or the last ended: session() declares IPMX, or no longer
    }
}

void Sender::set_sender_clock(bool const sender)
{
    if (sender == sender_clock_) {
        return;
    }
    sender_clock_ = sender;
    bump_session_version();
    ++info_block_version_;
}

void Sender::set_channel_order(std::string order)
{
    if (order == channel_order_) {
        return;
    }
    channel_order_ = std::move(order);
    ++info_block_version_;
    if (st2110_) {
        bump_session_version();  // session() names it too
    }
}

void Sender::set_st2110(bool const st2110)
{
    if (st2110 == st2110_) {
        return;
    }
    st2110_ = st2110;
    bump_session_version();
}

auto Sender::channel_order() const -> std::string
{
    if (!channel_order_.empty()) {
        return channel_order_;
    }
    std::string order = "SMPTE2110.(";
    for (uint32_t left = format_.channels; left > 0;) {
        uint32_t const group = std::min<uint32_t>(left, 64);  // U01 to U64
        std::format_to(std::back_inserter(order), "{}U{:02}", order.back() == '(' ? "" : ",", group);
        left -= group;
    }
    return order + ")";
}

auto Sender::described_channel_order() const -> std::string
{
    auto order = channel_order();
    return sdp::channel_order_channels(order) == format_.channels ? order : std::string{};
}

auto Sender::rtcp_options() const noexcept -> std::optional<RtcpOptions>
{
    if (!port_) {
        return std::nullopt;
    }
    return RtcpOptions{
        .destination = rtcp_endpoint_for({.address = destination_.address, .port = destination_.port}),
        .local = {},
        .ttl = destination_.ttl,
        .dscp = dscp_,
    };
}

void Sender::tick(uint64_t const now_index, uint64_t const tick_ns) noexcept
{
    if (!port_) {
        return;
    }
    tick_ns_ = tick_ns;
    if (paused_) {
        // A new run after a pause: a new SSRC and sequence (RFC 3550 Section 8), its SR counts afresh.
        paused_ = false;
        schedule_.renew();
        rtcp_counts_ = {};
    }
    // A transport (or frame port) that cannot get packets out puts the sender in error until it can.
    if (auto const send_error = stream_error()) {
        state_ = State::error;
        error_ = send_error;
    } else if (state_ == State::error) {
        state_ = State::ready;
        error_.clear();
    }
    if (frames_ != nullptr) [[unlikely]] {
        tick_frames(now_index);
        return;
    }
    if (auto const newest = newest_due(now_index)) {
        tell_send_offset();
        if (gathered_.load(std::memory_order_relaxed)) [[unlikely]] {
            resend_if_asked(*newest);
        }
        send_until(*newest);
    }
}

void Sender::resend_if_asked(uint64_t const newest) noexcept
{
    if (auto const from = port_->resend_from()) {
        // What went nowhere is not counted sent (counted again as it goes); what is left behind is lost; what a resync
        // counted lost and goes now is not.
        auto const rewound = schedule_.rewind(*from, newest);
        uncount_sent(static_cast<uint32_t>(rewound.unsent / std::max<uint32_t>(format_.frames_per_packet, 1)));
        counters_.lost = counters_.lost + rewound.lost - std::min(counters_.lost + rewound.lost, rewound.recovered);
        lost_elsewhere_ += rewound.lost;
    }
}

void Sender::uncount_sent(uint32_t const packets) noexcept
{
    uint64_t const frames = uint64_t{packets} * format_.frames_per_packet;
    counters_.packets -= std::min<uint64_t>(counters_.packets, packets);
    counters_.frames -= std::min(counters_.frames, frames);
    rtcp_counts_.packets -= std::min(rtcp_counts_.packets, packets);
    rtcp_counts_.octets -= std::min(rtcp_counts_.octets, static_cast<uint32_t>(packets * payload_.size()));
}

void Sender::tell_send_offset() noexcept
{
    uint32_t const send_offset = send_offset_.load(std::memory_order_relaxed);
    if (send_offset != port_send_offset_) [[unlikely]] {
        port_send_offset_ = send_offset;
        port_->set_send_offset(send_offset);
    }
}

void Sender::place_held() noexcept
{
    uint64_t const first = schedule_.held().value_or(0);
    send_state_.store({.ssrc = ssrc(), .first_index = first, .next_index = first});
    if (frames_ != nullptr) {
        start_run(first);
    }
}

auto Sender::newest_due(uint64_t const now_index) noexcept -> std::optional<uint64_t>
{
    auto const newest =
        schedule_.newest_due(now_index, send_offset_.load(std::memory_order_relaxed), holds_for_reports(), counters_.lost);
    if (!newest && schedule_.clear_placed()) {
        place_held();
    }
    return newest;
}

void Sender::send_until(uint64_t const newest) noexcept
{
    uint32_t const run_ssrc = ssrc();
    uint32_t queued = 0;
    while (schedule_.next_index() <= newest) {
        if (send_packet(schedule_.header(destination_.payload_type, run_ssrc))) {
            ++queued;
        } else {
            ++counters_.dropped;
        }
        schedule_.advance();
    }
    flush(queued, run_ssrc);
}

auto Sender::send_packet(rtp::RtpHeader const& header) noexcept -> bool
{
    // The provider writes straight into the transport's TX buffer; only when the transport has none
    // to give (the packet is dropped) does it write the staging buffer, so its frames are still counted.
    std::optional<Supply> supply;
    PayloadRenderer const render = [this, &supply](std::span<uint8_t> const payload) noexcept {
        supply = render_payload(schedule_.next_index(), format_.frames_per_packet, payload);
        return true;
    };
    bool const sent = port_->send_rendered_at(header, payload_.size(), render, tick_ns_);
    Supply const supplied = supply ? *supply : render_payload(schedule_.next_index(), format_.frames_per_packet, payload_);
    if (supplied == Supply::missing) {
        counters_.lost += format_.frames_per_packet;
    }
    return sent;
}

void Sender::flush(uint32_t const queued, uint32_t const run_ssrc) noexcept
{
    auto const sent = static_cast<uint32_t>(queued - port_->flush());
    if (gathered_.load(std::memory_order_relaxed)) [[unlikely]] {
        count_port_lost(port_->progress());
    }
    counters_.dropped += queued - sent;
    counters_.packets += sent;
    counters_.frames += uint64_t{sent} * format_.frames_per_packet;
    report_sent(sent, run_ssrc);
    if (sent > 0 && state_ != State::error) {
        state_ = State::running;
    }
}

void Sender::report_sent(uint32_t const packets, uint32_t const run_ssrc) noexcept
{
    rtcp_counts_.packets += packets;
    rtcp_counts_.octets += static_cast<uint32_t>(packets * payload_.size());
    rtcp_counts_.sent = rtcp_counts_.sent || packets != 0;
    send_state_.store({
        .ssrc = run_ssrc,
        .packets = rtcp_counts_.packets,
        .octets = rtcp_counts_.octets,
        .first_index = schedule_.run_first(),
        .next_index = schedule_.next_index(),
        .sent = rtcp_counts_.sent,
    });
}

void Sender::pause() noexcept
{
    if (!port_) {
        return;
    }
    if (!paused_) {
        paused_ = true;
        schedule_.stop();
        send_state_.store({});  // the RTCP session reads an SSRC of 0 as not sending
        stop_run();
    }
    if (auto const send_error = stream_error()) {
        state_ = State::error;
        error_ = send_error;
    } else {
        state_ = State::idle;
        error_.clear();
    }
}

auto Sender::render_payload(uint64_t const first_index, uint32_t const frames, std::span<uint8_t> const payload) noexcept -> Supply
{
    Supply const supply = provider_ ? provider_(first_index, frames, payload) : Supply::none;
    if (supply != Supply::frames) {
        fill_silence(format_.encoding, payload);
    }
    return supply;
}

void Sender::tick_frames(uint64_t const now_index) noexcept
{
    auto const newest = newest_due(now_index);  // a held run's placement starts the port's run there
    if (!newest) {
        return;
    }
    if (!run_started_ || run_first_index_ != schedule_.run_first()) {
        start_run(schedule_.run_first());  // not held, or a hold ended before its packet: from the newest due packet
    }
    follow_send_offset();
    write_until(*newest);
}

void Sender::start_run(uint64_t const first_index) noexcept
{
    stop_run();
    run_send_offset_ = send_offset_.load(std::memory_order_relaxed);
    run_first_index_ = first_index;
    frames_->start(
        {.ssrc = ssrc(), .first_sequence = schedule_.sequence(), .first_index = first_index, .send_offset = run_send_offset_});
    run_started_ = true;
}

void Sender::stop_run() noexcept
{
    if (run_started_) {
        frames_->stop();
        run_started_ = false;
    }
}

void Sender::follow_send_offset() noexcept
{
    uint32_t const send_offset = send_offset_.load(std::memory_order_relaxed);
    if (send_offset != run_send_offset_) {
        run_send_offset_ = send_offset;
        frames_->set_send_offset(send_offset);
    }
}

void Sender::write_until(uint64_t const newest) noexcept
{
    uint32_t written = 0;
    while (schedule_.next_index() <= newest) {
        if (write_packet()) {
            ++written;
        } else {
            ++counters_.dropped;
        }
        schedule_.advance();
    }
    count_written(written);
}

auto Sender::write_packet() noexcept -> bool
{
    // The provider writes straight into the port where the packet's frames lie in one piece; else into the staging
    // buffer, copied into the port's pieces after (where its storage wraps) or dropped (no room), its frames counted.
    uint64_t const first = schedule_.next_index();
    auto const out = frames_->writable(first, format_.frames_per_packet);
    bool const whole = out.size() >= payload_.size();
    if (render_payload(first, format_.frames_per_packet, whole ? out.first(payload_.size()) : std::span{payload_}) ==
        Supply::missing) {
        counters_.lost += format_.frames_per_packet;
    }
    return (whole || copy_staged(first)) && frames_->commit(first, format_.frames_per_packet);
}

auto Sender::copy_staged(uint64_t const first_index) noexcept -> bool
{
    size_t const frame_bytes = format_.bytes_per_frame();
    for (uint32_t done = 0; done < format_.frames_per_packet;) {
        auto const out = frames_->writable(first_index + done, format_.frames_per_packet - done);
        auto const n = static_cast<uint32_t>(std::min<size_t>(out.size() / frame_bytes, format_.frames_per_packet - done));
        if (n == 0) {
            return false;
        }
        statusbar::span_copy(out, std::span<uint8_t const>{payload_}.subspan(done * frame_bytes, n * frame_bytes));
        done += n;
    }
    return true;
}

void Sender::count_written(uint32_t const packets) noexcept
{
    counters_.packets += packets;
    counters_.frames += uint64_t{packets} * format_.frames_per_packet;
    auto const progress = run_progress();
    count_port_lost(progress);
    send_state_.store({
        .ssrc = ssrc(),
        .packets = progress.packets,
        .octets = static_cast<uint32_t>(uint64_t{progress.packets} * payload_.size()),
        .first_index = run_first_index_,
        .next_index = progress.next_index,
        .sent = progress.packets != 0,
    });
    if (packets > 0 && state_ != State::error) {
        state_ = State::running;
    }
}

void Sender::count_port_lost(SendProgress const& progress) noexcept
{
    bool const same = progress.ssrc == port_lost_.ssrc && progress.first_index == port_lost_.first_index;
    auto const found = static_cast<uint32_t>(progress.lost - (same ? port_lost_.lost : 0U));
    if (found >= (1U << 31U)) {
        return;  // behind the last reading: nothing new
    }
    counters_.lost += found;
    lost_elsewhere_ += found;
    port_lost_ = progress;
}

auto Sender::run_progress() const noexcept -> SendProgress
{
    auto const progress = frames_->progress();
    if (progress.ssrc == ssrc() && progress.first_index == run_first_index_) {
        return progress;
    }
    return {.ssrc = ssrc(), .first_index = run_first_index_, .next_index = run_first_index_, .packets = 0};  // not accepted yet
}

auto Sender::session() const -> std::optional<sdp::Session>
{
    if (!port_) {
        return std::nullopt;
    }
    statusbar::ip::IPv4Address const origin = transport_.local_address();
    bool const multicast = destination_.address.is_multicast();
    return sdp::Session{
        .session_id = session_id_,
        .session_version = session_version_,
        .origin_address = statusbar::ip::to_string(origin),
        .name = name_.empty() ? std::string{"-"} : name_,
        .connection = sdp::Connection{.address = destination_.address, .ttl = multicast ? destination_.ttl : uint8_t{0}},
        .attributes = attributes_,
        .media = {media_description(origin, multicast)},
    };
}

auto Sender::media_description(statusbar::ip::IPv4Address const& origin, bool const multicast) const -> sdp::Media
{
    return sdp::Media{
        .port = destination_.port,
        .payload_type = destination_.payload_type,
        .encoding = format_.encoding,
        .sample_rate = format_.sample_rate,
        .channels = format_.channels,
        .packet_time_us = format_.packet_time_us(),
        .max_packet_time_us = st2110_ ? std::optional{format_.packet_time_us()} : std::nullopt,
        .direction = sdp::Direction::recvonly,
        .clocks = references(),
        .media_clock = media_clock(),
        .source_filter =
            multicast ? std::optional{sdp::SourceFilter{.destination = destination_.address, .source = origin}} : std::nullopt,
        .clock_domain = clock_domain(),
        .channel_order = st2110_ ? described_channel_order() : std::string{},
        .ipmx = holds_for_reports(),
    };
}

}  // namespace padl::aes67
