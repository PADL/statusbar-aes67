// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/sdp/sdp_session.hpp"
#include "padl/sdp/sdp_text.hpp"
#include "statusbar/ip/ip_ipv4_address_format.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace padl::sdp {

namespace {

// std::format and append only: libc++ 20 format_to into a back_insert_iterator overflows (llvm-project issue 154670).

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

/// An attribute value as RFC 8866 Section 9's byte-string: CR and LF become spaces, NUL is dropped.
auto byte_string(std::string_view const text) -> std::string
{
    std::string value = one_line(text);
    std::erase(value, '\0');
    return value;
}

/// A token-char (RFC 8866 Section 9): a visible ASCII character other than `"(),/:;<=>?@[\]`.
constexpr auto is_token_char(char const c) noexcept -> bool
{
    constexpr std::string_view excluded{"\"(),/:;<=>?@[\\]"};
    auto const u = static_cast<unsigned char>(c);
    return u > ' ' && u < 0x7F && excluded.find(c) == std::string_view::npos;
}

/// An o= username or unicast address as read, else `fallback` when it is empty or holds a space, CR or LF: so a
/// re-served SDP keeps its origin tuple byte for byte. Not held to the token rule attribute names are (is_sdp_token()).
auto origin_field(std::string_view const text, std::string_view const fallback) -> std::string_view
{
    bool const kept = !text.empty() && text.find_first_of(" \r\n") == std::string_view::npos;
    return kept ? text : fallback;
}

/// Whether `text` is a token (RFC 8866 Section 9), as an attribute name must be (Section 5.13).
[[nodiscard]] auto is_sdp_token(std::string_view const text) noexcept -> bool
{
    return !text.empty() && std::ranges::all_of(text, is_token_char);
}

/// The line `<prefix><value>`, appended to `text`.
void write_line(std::string& text, std::string_view const prefix, std::string_view const value)
{
    text.append(prefix).append(value).append("\r\n");
}

void write_connection(std::string& text, Connection const& connection)
{
    text += "c=IN IP4 " + statusbar::ip::to_string(connection.address);
    // RFC 8866 Section 5.7: a TTL only for, and always for, a multicast address
    if (connection.address.is_multicast()) {
        auto const ttl = connection.ttl != 0 ? connection.ttl : Connection::DEFAULT_MULTICAST_TTL;
        text += std::format("/{}", ttl);
    }
    text += "\r\n";
}

void write_source_filter(std::string& text, SourceFilter const& filter)
{
    auto const destination = filter.destination ? statusbar::ip::to_string(*filter.destination) : std::string{"*"};
    write_line(text, "a=source-filter: incl IN IP4 ", destination + " " + statusbar::ip::to_string(filter.source));
}

/// `eui` in IEEE canonical form, upper case: "00-1D-C1-FF-FE-10-6C-16".
[[nodiscard]] auto canonical_eui64(statusbar::ieee::Eui64 const& eui) -> std::string
{
    auto const g = eui.span();
    return std::format("{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
}

/// RFC 7273: each PTP reference as ts_refclk_value() gives it (one it leaves empty is not written), then each other
/// source as read.
void write_clocks(std::string& text, ReferenceClocks const& clocks)
{
    for (auto const& clock : clocks.ptp) {
        if (auto const value = ts_refclk_value(clock); !value.empty()) {
            write_line(text, "a=ts-refclk:", value);
        }
    }
    for (auto const& value : clocks.other) {
        write_line(text, "a=ts-refclk:", one_line(value));
    }
}

/// `a=<attribute>:` and a packet time in milliseconds, with no trailing zeros: 1000 -> "1", 125 -> "0.125".
void write_ptime(std::string& text, uint32_t const micros, std::string_view const attribute = "ptime")
{
    text.append("a=").append(attribute).append(std::format(":{}", micros / 1000));
    if (uint32_t fraction = micros % 1000; fraction != 0) {
        int digits = 3;
        while (fraction % 10 == 0) {
            fraction /= 10;
            --digits;
        }
        text += std::format(".{:0{}}", fraction, digits);
    }
    text += "\r\n";
}

void write_rtpmap(std::string& text, PayloadFormat const& format)
{
    text += std::format(
        "a=rtpmap:{} {}/{}/{}\r\n", format.payload_type, encoding_name(format.encoding), format.sample_rate, format.channels);
}

/// The `i=` and `c=` lines of a session or media level, each when present.
template <typename Level>
void write_information_and_connection(std::string& text, Level const& level)
{
    if (!level.information.empty()) {
        write_line(text, "i=", one_line(level.information));
    }
    if (level.connection) {
        write_connection(text, *level.connection);
    }
}

/// The direction attribute, when set.
void write_direction(std::string& text, std::optional<Direction> const direction)
{
    if (direction) {
        write_line(text, "a=", direction_name(*direction));
    }
}

/// The clock and source attributes of a session or media level, each when present (RFC 7273, RFC 4570).
template <typename Level>
void write_clock_attributes(std::string& text, Level const& level)
{
    write_clocks(text, level.clocks);
    if (level.media_clock) {
        write_line(text, "a=mediaclk:", media_clock_value(*level.media_clock));
    }
    if (level.clock_domain) {
        text += std::format("a=clock-domain:PTPv2 {}\r\n", *level.clock_domain);
    }
    if (level.source_filter) {
        write_source_filter(text, *level.source_filter);
    }
}

/// The `m=` line: the preferred payload type, then the alternatives.
void write_media_line(std::string& text, Media const& media)
{
    text += std::format("m=audio {} RTP/AVP {}", media.port, media.payload_type);
    for (auto const& alternative : media.other_formats()) {
        text += std::format(" {}", alternative.payload_type);
    }
    text += "\r\n";
}

/// The primary format's fmtp: its channel order (ST 2110-30 Section 6.2.2), then `IPMX` (VSF TR-10-1 Section 10.1); none
/// without either. Text appended, std::format for the payload type alone.
void write_fmtp(std::string& text, Media const& media)
{
    bool const ordered = channel_order_channels(media.channel_order).has_value();
    if (!ordered && !media.ipmx) {
        return;
    }
    text += std::format("a=fmtp:{} ", media.payload_type);
    if (ordered) {
        text.append("channel-order=").append(media.channel_order).append(media.ipmx ? "; " : "");
    }
    text.append(media.ipmx ? "IPMX\r\n" : "\r\n");
}

/// An rtpmap for each payload type, in order of preference, the primary's fmtp, then ptime and maxptime.
void write_formats(std::string& text, Media const& media)
{
    write_rtpmap(text, media.format());
    for (auto const& alternative : media.other_formats()) {
        write_rtpmap(text, alternative);
    }
    write_fmtp(text, media);
    write_ptime(text, media.packet_time_us);
    if (media.max_packet_time_us && *media.max_packet_time_us != 0) {
        write_ptime(text, *media.max_packet_time_us, "maxptime");
    }
}

/// `a=rtcp:<port>[ IN IP4 <address>]` (RFC 3605).
void write_rtcp(std::string& text, RtcpAttribute const& rtcp)
{
    text += std::format("a=rtcp:{}", rtcp.port);
    if (rtcp.address) {
        text += " IN IP4 " + statusbar::ip::to_string(*rtcp.address);
    }
    text += "\r\n";
}

void write_media(std::string& text, Media const& media)
{
    write_media_line(text, media);
    write_information_and_connection(text, media);
    write_direction(text, media.direction);
    write_formats(text, media);
    write_clock_attributes(text, media);
    if (media.rtcp) {
        write_rtcp(text, *media.rtcp);
    }
}

/// Each extension attribute whose name is a token and not reserved (is_reserved_attribute()), in order:
/// `a=<name>:<value>` (byte_string()), or `a=<name>` without a value.
void write_attributes(std::string& text, std::span<Attribute const> const attributes)
{
    for (auto const& attribute : attributes) {
        if (!is_sdp_token(attribute.name) || is_reserved_attribute(attribute.name)) {
            continue;
        }
        auto const value = byte_string(attribute.value);
        write_line(text, "a=", value.empty() ? attribute.name : attribute.name + ":" + value);
    }
}

/// The `v=`, `o=` and `s=` lines.
void write_origin(std::string& text, Session const& session)
{
    text.append("v=0\r\no=").append(origin_field(session.username, "-"));
    text += std::format(" {} {}", session.session_id, session.session_version);
    write_line(text, " IN IP4 ", origin_field(session.origin_address, "0.0.0.0"));
    write_line(text, "s=", session.name.empty() ? std::string{"-"} : one_line(session.name));
}

/// The session level: the `v=`, `o=` and `s=` lines, `i=`, `c=`, `t=`, then the session attributes, extensions last.
void write_session_level(std::string& text, Session const& session)
{
    write_origin(text, session);
    write_information_and_connection(text, session);
    text += "t=0 0\r\n";
    write_direction(text, session.direction);
    write_clock_attributes(text, session);
    write_attributes(text, session.attributes);
}

}  // namespace

auto is_reserved_attribute(std::string_view const name) noexcept -> bool
{
    return std::ranges::any_of(
        RESERVED_ATTRIBUTES, [name](std::string_view const reserved) { return detail::equal_ignoring_case(name, reserved); });
}

auto ts_refclk_value(PtpReference const& clock) -> std::string
{
    auto const version = ptp_version_name(clock.version);
    if ((!clock.traceable && !clock.grandmaster) || version.empty()) {
        return one_line(clock.text);  // as read, or nothing
    }
    if (clock.traceable) {
        return std::format("ptp={}:traceable", version);
    }
    std::string value = std::format("ptp={}:{}", version, canonical_eui64(*clock.grandmaster));
    if (clock.version != PtpVersion::ieee802_1as) {
        value += std::format(":{}", clock.domain);  // AES67-2023 Clause 8.2: none for 802.1AS
    }
    return value;
}

auto localmac_value(statusbar::ieee::Eui48 const& mac) -> std::string
{
    auto const& v = mac.value;
    return std::format("localmac={:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}", v[0], v[1], v[2], v[3], v[4], v[5]);
}

auto ts_refclk_text(ReferenceClocks const& clocks) -> std::string
{
    std::string text;
    auto const add = [&text](std::string const& value) { text += (text.empty() || value.empty() ? "" : " ") + value; };
    for (auto const& clock : clocks.ptp) {
        add(ts_refclk_value(clock));
    }
    for (auto const& value : clocks.other) {
        add(one_line(value));
    }
    return text.empty() ? std::string{"no ts-refclk"} : text;
}

auto media_clock_value(MediaClock const& clock) -> std::string
{
    return clock.direct_offset ? std::format("direct={}", *clock.direct_offset) : std::string{"sender"};
}

auto to_string(Session const& session) -> std::string
{
    std::string text;
    write_session_level(text, session);
    for (auto const& media : session.media) {
        write_media(text, media);
    }
    return text;
}

}  // namespace padl::sdp
