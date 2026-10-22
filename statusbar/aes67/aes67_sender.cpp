// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_sender.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/ip/ip_ipv4_address_format.hpp"
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/rtp/rtp_header.hpp"

#include <algorithm>
#include <charconv>
#include <random>

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
    auto port = transport_.open_send({.destination = {destination.address, destination.port}, .ttl = destination.ttl, .dscp = 46});
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
        session_id_ = (uint64_t{random()} << 32U) | random();
        session_id_ &= 0x7FFF'FFFF'FFFF'FFFFULL;  // positive in any signed reading
    }
    ++session_version_;
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
    auto const& media = session.media.front();
    auto const connection = session.effective_connection(media);
    if (!connection) {
        return failure(Aes67Error::no_connection);
    }
    if (media.encoding != format_.encoding || media.channels != format_.channels || media.sample_rate != format_.sample_rate) {
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
    read_delay_ = previous.read_delay_;
    grandmaster_ = previous.grandmaster_;
    session_id_ = previous.session_id_;
    session_version_ = previous.session_version_;
}

void Sender::set_grandmaster(std::optional<Grandmaster> const& grandmaster)
{
    grandmaster_ = grandmaster;
}

void Sender::tick(uint64_t const now_index) noexcept
{
    if (!port_) {
        return;
    }
    uint64_t const fpp = format_.frames_per_packet;
    if (now_index + 1 < uint64_t{read_delay_} + fpp) {
        return;  // no packet due yet (only near the epoch)
    }
    // The newest packet whose last frame is read_delay_ or more behind now.
    uint64_t const newest = (((now_index + 1 - read_delay_) / fpp) - 1) * fpp;
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
        Supply const supply = provider_ ? provider_(next_index_, static_cast<uint32_t>(fpp), payload_) : Supply::none;
        if (supply != Supply::frames) {
            std::ranges::fill(payload_, uint8_t{0});
            if (supply == Supply::missing) {
                counters_.lost += fpp;
            }
        }
        rtp::RtpHeader header{};
        header.init(destination_.payload_type, sequence_, mediaclock::PtpMediaClock::rtp_timestamp(next_index_, 0), ssrc_);
        if (port_->send(header, payload_)) {
            ++queued;
        } else {
            ++counters_.dropped;
        }
        ++sequence_;
        next_index_ += fpp;
    }
    size_t const refused = port_->flush();
    counters_.dropped += refused;
    counters_.packets += queued - refused;
    counters_.frames += (queued - refused) * fpp;
    if (queued > refused) {
        state_ = State::running;
    }
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
    media.media_clock = sdp::MediaClock{.direct_offset = 0};
    if (multicast) {
        media.source_filter = sdp::SourceFilter{.destination = destination_.address, .source = origin};
    }
    session.media.push_back(std::move(media));
    return session;
}

}  // namespace statusbar::aes67
