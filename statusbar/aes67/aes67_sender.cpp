// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_sender.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/ip/ip_ipv4_address_format.hpp"
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/rtp/rtp_header.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <iterator>
#include <random>
#include <string>
#include <utility>

namespace statusbar::aes67 {

namespace {

/// A grandmaster's text split: an EUI-64 of 23 characters with separators (which may be ':') or 16
/// without, then any `:<domain>`.
struct GrandmasterText
{
    std::string_view eui;
    std::optional<std::string_view> domain;
};

[[nodiscard]] auto split_grandmaster(std::string_view const text) noexcept -> GrandmasterText
{
    for (size_t const length : {size_t{23}, size_t{16}}) {
        if (text.size() > length && text[length] == ':') {
            return {.eui = text.substr(0, length), .domain = text.substr(length + 1)};
        }
    }
    return {.eui = text, .domain = std::nullopt};
}

/// A PTP domain number, 0 to 127; std::nullopt otherwise.
[[nodiscard]] auto parse_domain(std::string_view const text) noexcept -> std::optional<uint8_t>
{
    unsigned value = 0;
    auto const [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value > 127) {
        return std::nullopt;
    }
    return static_cast<uint8_t>(value);
}

/// Fails unsupported_format unless the media clock supports the rate and a packet holds frames,
/// packet_too_large when a packet would not fit.
[[nodiscard]] auto check_send_format(StreamFormat const& format) noexcept -> Status
{
    if (!mediaclock::PtpMediaClock::supports(format.sample_rate) || format.frames_per_packet == 0 || format.channels == 0) {
        return failure(Aes67Error::unsupported_format);
    }
    if (format.bytes_per_packet() > Sender::MAX_PAYLOAD) {
        return failure(Aes67Error::packet_too_large);
    }
    return success();
}

}  // namespace

auto parse_grandmaster(std::string_view const text) -> std::optional<Grandmaster>
{
    auto const [eui, domain_text] = split_grandmaster(text);
    auto const identity = ieee::eui64_from_string(eui);
    auto const domain = domain_text ? parse_domain(*domain_text) : std::optional<uint8_t>{0};
    if (!identity || !domain) {
        return std::nullopt;
    }
    return Grandmaster{.identity = *identity, .domain = *domain};
}

Sender::Sender(StreamFormat const& format, FrameProvider provider, Transport& transport, std::string name)
    : format_{format}
    , provider_{std::move(provider)}
    , transport_{transport}
    , name_{std::move(name)}
    , send_offset_{default_send_offset(format)}
{}

auto Sender::configure(Destination const& destination) -> Status
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
    return success();
}

auto Sender::configure(sdp::Session const& session) -> Status
{
    auto const media = select_media(session, 0, format_.encoding);
    if (!media) {
        return forward_failure(media);
    }
    auto const connection = session.effective_connection(*media);
    if (!connection) {
        return failure(Aes67Error::no_connection);
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

auto Sender::open_port(Destination const& destination) -> Status
{
    clear();
    auto port = transport_.open_send({
        .destination = {.address = destination.address, .port = destination.port},
        .ttl = destination.ttl,
        .dscp = dscp_,
        .vlan = vlan_,
    });
    if (!port) {
        state_ = State::error;
        error_ = port.error();
        return failure(port.error());
    }
    port_ = std::move(*port);
    destination_ = destination;
    return success();
}

auto Sender::set_vlan(std::optional<ieee::VlanTag> const& vlan) noexcept -> Status
{
    if (port_ != nullptr) {
        if (auto const set = port_->set_vlan(vlan); !set) {
            return set;
        }
    }
    vlan_ = vlan;
    return success();
}

void Sender::seed_run()
{
    std::random_device random;
    payload_.assign(format_.bytes_per_packet(), 0);
    ssrc_.store(random(), std::memory_order_relaxed);
    rng_ = ((uint64_t{random()} << 32U) | random()) | 1U;
    sequence_ = static_cast<uint16_t>(random());
    rtcp_counts_ = {};
}

void Sender::renew_session()
{
    if (session_id_ == 0) {
        session_id_ = stable_session_id(std::format("{}/{}", ip::to_string(transport_.local_address()), name_));
    }
    bump_session_version();
    ++info_block_version_;
}

void Sender::become_ready() noexcept
{
    send_state_.store({});  // not ticked while configured, so tick() is not storing too
    paused_ = false;
    started_ = false;
    state_ = State::ready;
    error_.clear();
}

void Sender::clear() noexcept
{
    port_.reset();
    started_ = false;
    state_ = State::idle;
}

void Sender::continue_from(Sender const& previous)
{  // NOLINT(statusbar-block-statements): the settings carried over, field by field
    send_offset_.store(previous.send_offset(), std::memory_order_relaxed);
    grandmaster_ = previous.grandmaster_;
    sender_clock_ = previous.sender_clock_;
    name_ = previous.name_;
    session_id_ = previous.session_id_;
    session_version_ = previous.session_version_;
    dscp_ = previous.dscp_;
    channel_order_ = previous.channel_order_;
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

void Sender::set_grandmaster(std::optional<Grandmaster> const& grandmaster)
{
    if (grandmaster == grandmaster_) {
        return;
    }
    grandmaster_ = grandmaster;
    bump_session_version();  // the SDP changed: a receiver keying on o= sees a new version
    ++info_block_version_;
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

auto Sender::rtcp_options() const noexcept -> std::optional<RtcpOptions>
{
    if (!port_) {
        return std::nullopt;
    }
    return RtcpOptions{
        .destination = rtcp_endpoint_of({.address = destination_.address, .port = destination_.port}),
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
        ssrc_.store(fresh_ssrc(), std::memory_order_relaxed);
        sequence_ = static_cast<uint16_t>(next_random());
        rtcp_counts_ = {};
    }
    // A transport that cannot get packets out puts the sender in error until it can.
    if (auto const send_error = transport_.send_error()) {
        state_ = State::error;
        error_ = send_error;
    } else if (state_ == State::error) {
        state_ = State::ready;
        error_.clear();
    }
    if (auto const newest = newest_due(now_index)) {
        send_until(*newest);
    }
}

auto Sender::newest_due(uint64_t const now_index) noexcept -> std::optional<uint64_t>
{
    uint64_t const fpp = format_.frames_per_packet;
    uint64_t const send_offset = send_offset_.load(std::memory_order_relaxed);
    uint64_t const phase = phase_;
    if (now_index + 1 < send_offset + fpp + phase) {
        return std::nullopt;  // no packet due yet (only near the epoch)
    }
    // The newest packet (on the grid shifted by the phase) whose last frame is send_offset or more behind now.
    uint64_t const newest = ((((now_index + 1 - send_offset - phase) / fpp) - 1) * fpp) + phase;
    return resync(newest) ? std::optional{newest} : std::nullopt;
}

auto Sender::resync(uint64_t const newest) noexcept -> bool
{
    uint64_t const fpp = format_.frames_per_packet;
    if (!started_) {
        next_index_ = newest;
        started_ = true;
    } else if (next_index_ > newest + fpp) {
        // The clock went back. A small step is waited out (nothing is due
        // until now catches up, so no index is sent twice); a large one,
        // more than MAX_BURST packets, resynchronises.
        if (next_index_ - (newest + fpp) <= uint64_t{MAX_BURST} * fpp) {
            return false;
        }
        next_index_ = newest;
    } else if (next_index_ + (uint64_t{MAX_BURST} * fpp) <= newest) {
        counters_.lost += newest - next_index_;  // too far behind: skip what cannot be sent in time
        next_index_ = newest;
    }
    return true;
}

void Sender::send_until(uint64_t const newest) noexcept
{
    uint32_t const run_ssrc = ssrc();
    uint32_t queued = 0;
    while (next_index_ <= newest) {
        rtp::RtpHeader header{};
        header.init(destination_.payload_type, sequence_, mediaclock::PtpMediaClock::rtp_timestamp(next_index_, 0), run_ssrc);
        if (send_packet(header)) {
            ++queued;
        } else {
            ++counters_.dropped;
        }
        ++sequence_;
        next_index_ += format_.frames_per_packet;
    }
    flush(queued, run_ssrc);
}

auto Sender::send_packet(rtp::RtpHeader const& header) noexcept -> bool
{
    // The provider writes straight into the transport's TX buffer; only when the transport has none
    // to give (the packet is dropped) does it write the staging buffer, so its frames are still counted.
    std::optional<Supply> supply;
    PayloadRenderer const render = [this, &supply](std::span<uint8_t> const payload) noexcept {
        supply = render_payload(next_index_, payload);
        return true;
    };
    bool const sent = port_->send_rendered_at(header, payload_.size(), render, tick_ns_);
    Supply const supplied = supply ? *supply : render_payload(next_index_, payload_);
    if (supplied == Supply::missing) {
        counters_.lost += format_.frames_per_packet;
    }
    return sent;
}

void Sender::flush(uint32_t const queued, uint32_t const run_ssrc) noexcept
{
    auto const sent = static_cast<uint32_t>(queued - port_->flush());
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
    send_state_.store({.ssrc = run_ssrc, .packets = rtcp_counts_.packets, .octets = rtcp_counts_.octets});
}

void Sender::pause() noexcept
{
    if (!port_) {
        return;
    }
    if (!paused_) {
        paused_ = true;
        started_ = false;
        send_state_.store({});  // the RTCP session takes an SSRC of 0 as not sending
    }
    if (auto const send_error = transport_.send_error()) {
        state_ = State::error;
        error_ = send_error;
    } else {
        state_ = State::idle;
        error_.clear();
    }
}

auto Sender::next_random() noexcept -> uint32_t
{
    rng_ ^= rng_ >> 12U;
    rng_ ^= rng_ << 25U;
    rng_ ^= rng_ >> 27U;
    return static_cast<uint32_t>((rng_ * 0x2545'F491'4F6C'DD1DULL) >> 32U);
}

auto Sender::fresh_ssrc() noexcept -> uint32_t
{
    uint32_t const old = ssrc();
    uint32_t next = next_random();
    while (next == 0 || next == old) {
        next = next_random();
    }
    return next;
}

auto Sender::render_payload(uint64_t const first_index, std::span<uint8_t> const payload) noexcept -> Supply
{
    Supply const supply = provider_ ? provider_(first_index, format_.frames_per_packet, payload) : Supply::none;
    if (supply != Supply::frames) {
        fill_silence(format_.encoding, payload);
    }
    return supply;
}

auto Sender::session() const -> std::optional<sdp::Session>
{
    if (!port_) {
        return std::nullopt;
    }
    ip::IPv4Address const origin = transport_.local_address();
    bool const multicast = destination_.address.is_multicast();
    return sdp::Session{
        .session_id = session_id_,
        .session_version = session_version_,
        .origin_address = ip::to_string(origin),
        .name = name_.empty() ? std::string{"-"} : name_,
        .connection = sdp::Connection{.address = destination_.address, .ttl = multicast ? destination_.ttl : uint8_t{0}},
        .media = {media_description(origin, multicast)},
    };
}

auto Sender::media_description(ip::IPv4Address const& origin, bool const multicast) const -> sdp::Media
{
    return sdp::Media{
        .port = destination_.port,
        .payload_type = destination_.payload_type,
        .encoding = format_.encoding,
        .sample_rate = format_.sample_rate,
        .channels = format_.channels,
        .packet_time_us = format_.packet_time_us(),
        .direction = sdp::Direction::recvonly,
        .clocks = sdp::ReferenceClocks::of(grandmaster_.transform(&Grandmaster::reference)),
        .media_clock = media_clock(),
        .source_filter =
            multicast ? std::optional{sdp::SourceFilter{.destination = destination_.address, .source = origin}} : std::nullopt,
        .clock_domain = clock_domain(),
    };
}

}  // namespace statusbar::aes67
