// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/ip/ip_ipv4_address_format.hpp"
#include "statusbar/sdp/sdp.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
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
    // RFC 8866 §5.7: a TTL only for, and always for, a multicast address
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

/// RFC 7273: the grandmaster as an upper-case EUI-64 in IEEE canonical form, the domain bare (0
/// included, as Dante writes it); a reference with neither grandmaster nor traceable is not written.
void write_clock(Out out, PtpReference const& clock)
{
    if (auto const value = ts_refclk_value(clock); !value.empty()) {
        std::format_to(out, "a=ts-refclk:{}\r\n", value);
    }
}

/// RFC 7273 §5: a direct clock with its offset; any other is written as
/// `sender`, the media clock that is asynchronous to the reference clock.
void write_media_clock(Out out, MediaClock const& clock)
{
    std::format_to(out, "a=mediaclk:{}\r\n", media_clock_value(clock));
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

void write_rtpmap(Out out, PayloadFormat const& format)
{
    std::format_to(
        out, "a=rtpmap:{} {}/{}/{}\r\n", format.payload_type, encoding_name(format.encoding), format.sample_rate, format.channels);
}

/// The `i=` and `c=` lines of a session or media level, each when present.
template <typename Level>
void write_information_and_connection(Out out, Level const& level)
{
    if (!level.information.empty()) {
        out = std::format_to(out, "i={}\r\n", one_line(level.information));
    }
    if (level.connection) {
        write_connection(out, *level.connection);
    }
}

/// The direction attribute, when set.
void write_direction(Out out, std::optional<Direction> const direction)
{
    if (direction) {
        std::format_to(out, "a={}\r\n", direction_name(*direction));
    }
}

/// The clock and source attributes of a session or media level, each when present (RFC 7273, RFC 4570).
template <typename Level>
void write_clock_attributes(Out out, Level const& level)
{
    if (level.clock) {
        write_clock(out, *level.clock);
    }
    if (level.media_clock) {
        write_media_clock(out, *level.media_clock);
    }
    if (level.clock_domain) {
        out = std::format_to(out, "a=clock-domain:PTPv2 {}\r\n", *level.clock_domain);
    }
    if (level.source_filter) {
        write_source_filter(out, *level.source_filter);
    }
}

/// The `m=` line: the preferred payload type, then the alternatives.
void write_media_line(Out out, Media const& media)
{
    out = std::format_to(out, "m=audio {} RTP/AVP {}", media.port, media.payload_type);
    for (auto const& alternative : media.other_formats()) {
        out = std::format_to(out, " {}", alternative.payload_type);
    }
    std::format_to(out, "\r\n");
}

/// An rtpmap for each payload type, in order of preference, then the ptime.
void write_formats(Out out, Media const& media)
{
    write_rtpmap(out, media.format());
    for (auto const& alternative : media.other_formats()) {
        write_rtpmap(out, alternative);
    }
    write_ptime(out, media.packet_time_us);
}

/// `a=rtcp:<port>[ IN IP4 <address>]` (RFC 3605).
void write_rtcp(Out out, RtcpAttribute const& rtcp)
{
    out = std::format_to(out, "a=rtcp:{}", rtcp.port);
    if (rtcp.address) {
        out = std::format_to(out, " IN IP4 ");
        out = ip::format_to(out, *rtcp.address);
    }
    std::format_to(out, "\r\n");
}

void write_media(Out out, Media const& media)
{
    write_media_line(out, media);
    write_information_and_connection(out, media);
    write_direction(out, media.direction);
    write_formats(out, media);
    write_clock_attributes(out, media);
    if (media.rtcp) {
        write_rtcp(out, *media.rtcp);
    }
}

/// The session level: the `v=`, `o=` and `s=` lines, `i=`, `c=`, `t=`, then the session attributes.
void write_session_level(Out out, Session const& session)
{
    out = std::format_to(
        out,
        "v=0\r\no={} {} {} IN IP4 {}\r\ns={}\r\n",
        token(session.username, "-"),
        session.session_id,
        session.session_version,
        token(session.origin_address, "0.0.0.0"),
        session.name.empty() ? std::string{"-"} : one_line(session.name));
    write_information_and_connection(out, session);
    out = std::format_to(out, "t=0 0\r\n");
    write_direction(out, session.direction);
    write_clock_attributes(out, session);
}

}  // namespace

auto ts_refclk_value(PtpReference const& clock) -> std::string
{
    if (!clock.traceable && !clock.grandmaster) {
        return {};
    }
    std::string value = std::format("ptp={}", one_line(clock.version));
    if (clock.traceable) {
        value += ":traceable";
    } else {
        auto const g = clock.grandmaster->span();
        std::format_to(
            std::back_inserter(value),
            ":{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}:{}",
            g[0],
            g[1],
            g[2],
            g[3],
            g[4],
            g[5],
            g[6],
            g[7],
            clock.domain);
    }
    return value;
}

auto media_clock_value(MediaClock const& clock) -> std::string
{
    return clock.direct_offset ? std::format("direct={}", *clock.direct_offset) : std::string{"sender"};
}

auto to_string(Session const& session) -> std::string
{
    std::string text;
    write_session_level(std::back_inserter(text), session);
    for (auto const& media : session.media) {
        write_media(std::back_inserter(text), media);
    }
    return text;
}

}  // namespace statusbar::sdp
