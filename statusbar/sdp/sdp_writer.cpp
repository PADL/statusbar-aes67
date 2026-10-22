// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/ip/ip_ipv4_address_format.hpp"
#include "statusbar/sdp/sdp.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace statusbar::sdp {

namespace {

using Out = std::back_insert_iterator<std::string>;

auto direction_name(Direction const direction) noexcept -> char const*
{
    switch (direction) {
        case Direction::sendonly:
            return "sendonly";
        case Direction::recvonly:
            return "recvonly";
        case Direction::inactive:
            return "inactive";
        case Direction::sendrecv:
            break;
    }
    return "sendrecv";
}

/// A text field on one line: CR and LF would start new SDP lines.
auto one_line(std::string_view const text) -> std::string
{
    std::string line{text};
    std::ranges::replace(line, '\r', ' ');
    std::ranges::replace(line, '\n', ' ');
    return line;
}

/// A single non-empty token, or `fallback`: an o= username or unicast address.
auto token(std::string_view const text, std::string_view const fallback) -> std::string_view
{
    bool const is_token = !text.empty() && text.find_first_of(" \r\n") == std::string_view::npos;
    return is_token ? text : fallback;
}

void write_connection(Out out, Connection const& connection)
{
    out = std::format_to(out, "c=IN IP4 ");
    out = ip::format_to(out, connection.address);
    // RFC 8866 section 5.7: a TTL only for, and always for, a multicast address
    if (connection.address.is_multicast()) {
        auto const ttl = connection.ttl != 0 ? connection.ttl : Connection::DEFAULT_MULTICAST_TTL;
        out = std::format_to(out, "/{}", ttl);
    }
    std::format_to(out, "\r\n");
}

void write_source_filter(Out out, SourceFilter const& filter)
{
    out = std::format_to(out, "a=source-filter: incl IN IP4 ");
    if (filter.destination) {
        out = ip::format_to(out, *filter.destination);
    } else {
        out = std::format_to(out, "*");
    }
    out = std::format_to(out, " ");
    out = ip::format_to(out, filter.source);
    std::format_to(out, "\r\n");
}

/// RFC 7273: the grandmaster as an EUI-64 in IEEE canonical form, upper case;
/// the domain in the bare form AES67 devices write and read. A reference with
/// neither grandmaster nor traceable has no valid form, and is not written.
void write_clock(Out out, PtpReference const& clock)
{
    if (!clock.traceable && !clock.grandmaster) {
        return;
    }
    out = std::format_to(out, "a=ts-refclk:ptp={}", one_line(clock.version));
    if (clock.traceable) {
        out = std::format_to(out, ":traceable");
    } else if (clock.grandmaster) {
        auto const g = clock.grandmaster->span();
        out = std::format_to(
            out, ":{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
        if (clock.domain != 0) {
            out = std::format_to(out, ":{}", clock.domain);
        }
    }
    std::format_to(out, "\r\n");
}

/// RFC 7273 section 5: a direct clock with its offset; any other is written as
/// `sender`, the media clock that is asynchronous to the reference clock.
void write_media_clock(Out out, MediaClock const& clock)
{
    if (clock.direct_offset) {
        std::format_to(out, "a=mediaclk:direct={}\r\n", *clock.direct_offset);
    } else {
        std::format_to(out, "a=mediaclk:sender\r\n");
    }
}

/// A packet time in milliseconds, with no trailing zeros: 1000 -> "1", 125 -> "0.125".
void write_ptime(Out out, uint32_t const micros)
{
    out = std::format_to(out, "a=ptime:{}", micros / 1000);
    if (uint32_t fraction = micros % 1000; fraction != 0) {
        int digits = 3;
        while (fraction % 10 == 0) {
            fraction /= 10;
            --digits;
        }
        out = std::format_to(out, ".{:0{}}", fraction, digits);
    }
    std::format_to(out, "\r\n");
}

void write_media(Out out, Media const& media)
{
    out = std::format_to(out, "m=audio {} RTP/AVP {}\r\n", media.port, media.payload_type);
    if (!media.information.empty()) {
        out = std::format_to(out, "i={}\r\n", one_line(media.information));
    }
    if (media.connection) {
        write_connection(out, *media.connection);
    }
    if (media.direction) {
        out = std::format_to(out, "a={}\r\n", direction_name(*media.direction));
    }
    out = std::format_to(
        out, "a=rtpmap:{} {}/{}/{}\r\n", media.payload_type, encoding_name(media.encoding), media.sample_rate, media.channels);
    write_ptime(out, media.packet_time_us);
    if (media.clock) {
        write_clock(out, *media.clock);
    }
    if (media.media_clock) {
        write_media_clock(out, *media.media_clock);
    }
    if (media.source_filter) {
        write_source_filter(out, *media.source_filter);
    }
}

}  // namespace

auto to_string(Session const& session) -> std::string
{
    std::string text;
    auto out = std::back_inserter(text);
    out = std::format_to(
        out,
        "v=0\r\no={} {} {} IN IP4 {}\r\ns={}\r\n",
        token(session.username, "-"),
        session.session_id,
        session.session_version,
        token(session.origin_address, "0.0.0.0"),
        session.name.empty() ? std::string{"-"} : one_line(session.name));
    if (!session.information.empty()) {
        out = std::format_to(out, "i={}\r\n", one_line(session.information));
    }
    if (session.connection) {
        write_connection(out, *session.connection);
    }
    out = std::format_to(out, "t=0 0\r\n");
    if (session.direction) {
        out = std::format_to(out, "a={}\r\n", direction_name(*session.direction));
    }
    if (session.clock) {
        write_clock(out, *session.clock);
    }
    if (session.media_clock) {
        write_media_clock(out, *session.media_clock);
    }
    if (session.source_filter) {
        write_source_filter(out, *session.source_filter);
    }
    for (auto const& media : session.media) {
        write_media(out, media);
    }
    return text;
}

}  // namespace statusbar::sdp
