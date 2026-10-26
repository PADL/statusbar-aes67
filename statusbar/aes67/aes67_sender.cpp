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

auto parse_grandmaster(std::string_view const text) -> std::optional<Grandmaster>
{
    Grandmaster grandmaster{};
    std::string_view eui = text;
    // An EUI-64 is 23 characters with separators (which may be ':') or 16 without.
    for (size_t const length : {size_t{23}, size_t{16}}) {
        if (text.size() > length && text[length] == ':') {
            std::string_view const domain = text.substr(length + 1);
            unsigned value = 0;
            auto const [end, ec] = std::from_chars(domain.data(), domain.data() + domain.size(), value);
            if (ec != std::errc{} || end != domain.data() + domain.size() || value > 127) {
                return std::nullopt;
            }
            grandmaster.domain = static_cast<uint8_t>(value);
            eui = text.substr(0, length);
            break;
        }
    }
    auto const identity = ieee::eui64_from_string(eui);
    if (!identity) {
        return std::nullopt;
    }
    grandmaster.identity = *identity;
    return grandmaster;
}

Sender::Sender(StreamFormat const& format, FrameProvider provider, Transport& transport, std::string name)
    : format_{format}
    , provider_{std::move(provider)}
    , transport_{transport}
    , name_{std::move(name)}
    , read_delay_{default_read_delay(format)}
{}

auto Sender::configure(Destination const& destination) -> Status
{
    if (!mediaclock::PtpMediaClock::supports(format_.sample_rate) || format_.frames_per_packet == 0 || format_.channels == 0) {
        return failure(Aes67Error::unsupported_format);
    }
    if (format_.bytes_per_packet() > MAX_PAYLOAD) {
        return failure(Aes67Error::packet_too_large);
    }
    clear();
    auto port = transport_.open_send(
        {.destination = {.address = destination.address, .port = destination.port}, .ttl = destination.ttl, .dscp = dscp_});
    if (!port) {
        state_ = State::error;
        error_ = port.error();
        return failure(port.error());
    }
    port_ = std::move(*port);
    destination_ = destination;
    payload_.assign(format_.bytes_per_packet(), 0);

    std::random_device random;
    ssrc_ = random();
    sequence_ = static_cast<uint16_t>(random());
    if (session_id_ == 0) {
        std::string key = ip::to_string(transport_.local_address());
        key += '/';
        key += name_;
        session_id_ = stable_session_id(key);
    }
    bump_session_version();
    ++info_block_version_;
    rtcp_packets_ = 0;
    rtcp_octets_ = 0;
    send_state_.store({});  // not ticked while configured, so tick() is not storing too
    started_ = false;
    state_ = State::ready;
    error_.clear();
    return success();
}

auto Sender::configure(sdp::Session const& session) -> Status
{
    if (session.media.empty()) {
        return failure(Aes67Error::no_media);
    }
    auto media = session.media.front();
    (void)media.prefer(format_.encoding);  // else the primary, refused below
    auto const connection = session.effective_connection(media);
    if (!connection) {
        return failure(Aes67Error::no_connection);
    }
    if (media.encoding != format_.encoding) {
        return failure(Aes67Error::encoding_mismatch);
    }
    if (media.channels != format_.channels || media.sample_rate != format_.sample_rate) {
        return failure(Aes67Error::format_mismatch);
    }
    return configure(Destination{
        .address = connection->address,
        .port = media.port,
        .payload_type = media.payload_type,
        .ttl = connection->ttl != 0 ? connection->ttl : sdp::Connection::DEFAULT_MULTICAST_TTL,
    });
}

void Sender::clear() noexcept
{
    port_.reset();
    started_ = false;
    state_ = State::idle;
}

void Sender::continue_from(Sender const& previous)
{
    read_delay_.store(previous.read_delay(), std::memory_order_relaxed);
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

void Sender::tick(uint64_t const now_index) noexcept
{
    if (!port_) {
        return;
    }
    // A transport that cannot get packets out puts the sender in error until it can.
    if (auto const send_error = transport_.send_error()) {
        state_ = State::error;
        error_ = send_error;
    } else if (state_ == State::error) {
        state_ = State::ready;
        error_.clear();
    }
    uint64_t const fpp = format_.frames_per_packet;
    uint64_t const read_delay = read_delay_.load(std::memory_order_relaxed);
    uint64_t const phase = phase_;
    if (now_index + 1 < read_delay + fpp + phase) {
        return;  // no packet due yet (only near the epoch)
    }
    // The newest packet (on the grid shifted by the phase) whose last frame
    // is read_delay or more behind now.
    uint64_t const newest = ((((now_index + 1 - read_delay - phase) / fpp) - 1) * fpp) + phase;
    if (!started_) {
        next_index_ = newest;
        started_ = true;
    } else if (next_index_ > newest + fpp) {
        // The clock went back. A small step is waited out (nothing is due
        // until now catches up, so no index is sent twice); a large one,
        // more than MAX_BURST packets, resynchronises.
        if (next_index_ - (newest + fpp) <= uint64_t{MAX_BURST} * fpp) {
            return;
        }
        next_index_ = newest;
    } else if (next_index_ + (uint64_t{MAX_BURST} * fpp) <= newest) {
        counters_.lost += newest - next_index_;  // too far behind: skip what cannot be sent in time
        next_index_ = newest;
    }

    uint32_t queued = 0;
    while (next_index_ <= newest) {
        rtp::RtpHeader header{};
        header.init(destination_.payload_type, sequence_, mediaclock::PtpMediaClock::rtp_timestamp(next_index_, 0), ssrc_);
        // The provider writes straight into the transport's TX buffer; only
        // when the transport has none to give (the packet is dropped) does it
        // write the staging buffer, so its frames are still taken and counted.
        std::optional<Supply> supply;
        PayloadRenderer const render = [this, &supply](std::span<uint8_t> const payload) noexcept {
            supply = render_payload(next_index_, payload);
            return true;
        };
        if (port_->send_rendered(header, payload_.size(), render)) {
            ++queued;
        } else {
            ++counters_.dropped;
        }
        if (!supply) {
            supply = render_payload(next_index_, payload_);
        }
        if (*supply == Supply::missing) {
            counters_.lost += fpp;
        }
        ++sequence_;
        next_index_ += fpp;
    }
    size_t const refused = port_->flush();
    counters_.dropped += refused;
    counters_.packets += queued - refused;
    rtcp_packets_ += static_cast<uint32_t>(queued - refused);
    rtcp_octets_ += static_cast<uint32_t>((queued - refused) * payload_.size());
    send_state_.store({.ssrc = ssrc_, .packets = rtcp_packets_, .octets = rtcp_octets_});
    counters_.frames += (queued - refused) * fpp;
    if (queued > refused && state_ != State::error) {
        state_ = State::running;
    }
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

    sdp::Session session{};
    session.session_id = session_id_;
    session.session_version = session_version_;
    session.origin_address = ip::to_string(origin);
    session.name = name_.empty() ? std::string{"-"} : name_;
    session.connection = sdp::Connection{.address = destination_.address, .ttl = multicast ? destination_.ttl : uint8_t{0}};

    sdp::Media media{};
    media.port = destination_.port;
    media.payload_type = destination_.payload_type;
    media.encoding = format_.encoding;
    media.sample_rate = format_.sample_rate;
    media.channels = format_.channels;
    media.packet_time_us = format_.packet_time_us();
    media.direction = sdp::Direction::recvonly;
    if (grandmaster_) {
        // Always PTPv2 (PtpReference's default version), whatever the AVB side runs.
        media.clock = sdp::PtpReference{.grandmaster = grandmaster_->identity, .domain = grandmaster_->domain};
        media.clock_domain = grandmaster_->domain;
    }
    media.media_clock = media_clock();
    if (multicast) {
        media.source_filter = sdp::SourceFilter{.destination = destination_.address, .source = origin};
    }
    session.media.push_back(std::move(media));
    return session;
}

}  // namespace statusbar::aes67
