#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// SDP - RFC 8866 session descriptions, for AES67 audio streams.
///
/// Parses and writes the subset AES67 uses: an origin, a session name, IPv4
/// connection data (multicast TTL included), and audio media over RTP/AVP with
/// L16/L24 rtpmaps, ptime, the RFC 7273 reference clock and media clock
/// (`ts-refclk:ptp=...`, `mediaclk:direct=<offset>`), and RFC 4570 source
/// filters. A session may hold several audio media (ST 2022-7 sends the same
/// stream twice); other media are skipped. Attributes the bridge has no use for
/// are ignored. Connection, clock and media clock may be given at session or
/// media level; the effective_*() accessors resolve that.

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/sdp/sdp_error.hpp"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::sdp {

/// AES67 linear PCM encodings (RFC 3551 L16, RFC 3190 L24)
enum class Encoding : uint8_t
{
    l16,
    l24,
};

[[nodiscard]] constexpr auto bytes_per_sample(Encoding const encoding) noexcept -> uint32_t
{
    return encoding == Encoding::l16 ? 2 : 3;
}

/// `c=IN IP4 <address>[/<ttl>]`
struct Connection
{
    /// Written for a multicast address whose TTL is 0 (RFC 8866 requires one).
    static constexpr uint8_t DEFAULT_MULTICAST_TTL = 32;

    ip::IPv4Address address;
    uint8_t ttl{0};  ///< multicast TTL; 0 when absent (unicast, or a sender that left it out)

    auto operator==(Connection const&) const noexcept -> bool = default;
};

/// `a=ts-refclk:ptp=<version>[:<grandmaster>[:<domain>]]` or `ptp=<version>:traceable`
/// (RFC 7273 section 4.3). The domain may be written bare (`:0`, as AES67 devices
/// do) or as `domain-nmbr=0`; a 2002 `domain-name=` is read as domain 0.
struct PtpReference
{
    std::string version{"IEEE1588-2008"};    ///< or IEEE802.1AS-2011, ...
    std::optional<ieee::Eui64> grandmaster;  ///< nullopt when not given, or traceable
    bool traceable{false};
    uint8_t domain{0};

    auto operator==(PtpReference const&) const noexcept -> bool = default;
};

/// `a=source-filter: incl IN IP4 <destination> <source>` (RFC 4570), first source
struct SourceFilter
{
    std::optional<ip::IPv4Address> destination;  ///< nullopt for `*`: every connection address
    ip::IPv4Address source;

    auto operator==(SourceFilter const&) const noexcept -> bool = default;
};

enum class Direction : uint8_t
{
    sendrecv,
    sendonly,
    recvonly,
    inactive,
};

/// One `m=audio <port> RTP/AVP <pt>` section
struct Media
{
    uint16_t port{0};
    uint8_t payload_type{0};
    Encoding encoding{Encoding::l24};
    uint32_t sample_rate{48000};
    uint16_t channels{1};
    uint32_t packet_time_us{1000};  ///< a=ptime; AES67's default is 1 ms
    std::string information;        ///< i=
    std::optional<Direction> direction;
    std::optional<Connection> connection;
    std::optional<PtpReference> clock;
    std::optional<uint32_t> media_clock_offset;  ///< mediaclk:direct=, mod 2^32
    std::optional<SourceFilter> source_filter;

    /// Bytes in one frame: one sample of every channel.
    [[nodiscard]] constexpr auto frame_bytes() const noexcept -> uint32_t { return channels * bytes_per_sample(encoding); }

    /// Frames in one packet, from ptime rounded to whole frames (a sender may
    /// write 125 us as 0.12).
    [[nodiscard]] constexpr auto frames_per_packet() const noexcept -> uint32_t
    {
        return static_cast<uint32_t>(((uint64_t{packet_time_us} * sample_rate) + 500'000) / 1'000'000);
    }

    auto operator==(Media const&) const noexcept -> bool = default;
};

/// A session description
struct Session
{
    std::string username{"-"};
    uint64_t session_id{0};
    uint64_t session_version{0};
    ip::IPv4Address origin;
    std::string name{"-"};  ///< s=; RFC 8866 requires one, "-" when there is nothing to say
    std::string information;
    std::optional<Direction> direction;
    std::optional<Connection> connection;
    std::optional<PtpReference> clock;
    std::optional<uint32_t> media_clock_offset;
    std::optional<SourceFilter> source_filter;
    std::vector<Media> media;

    [[nodiscard]] auto effective_direction(Media const& m) const noexcept -> Direction
    {
        return m.direction.value_or(direction.value_or(Direction::sendrecv));
    }
    [[nodiscard]] auto effective_connection(Media const& m) const noexcept -> std::optional<Connection>
    {
        return m.connection ? m.connection : connection;
    }
    [[nodiscard]] auto effective_clock(Media const& m) const -> std::optional<PtpReference> { return m.clock ? m.clock : clock; }
    [[nodiscard]] auto effective_source_filter(Media const& m) const noexcept -> std::optional<SourceFilter>
    {
        return m.source_filter ? m.source_filter : source_filter;
    }
    /// RFC 7273: a media clock without an offset is direct=0.
    [[nodiscard]] auto effective_media_clock_offset(Media const& m) const noexcept -> uint32_t
    {
        return m.media_clock_offset.value_or(media_clock_offset.value_or(0));
    }

    auto operator==(Session const&) const -> bool = default;
};

/// Parse a session description. Lines may end in CRLF or LF. Fails unless it
/// has v=0, an IN IP4 origin, s= and t=, and every audio media kept has a
/// connection at media or session level. Audio media whose rtpmap is not L16 or
/// L24 are skipped; if that leaves none, the error is unsupported_encoding.
[[nodiscard]] auto parse(std::string_view text) -> StatusValue<Session>;

/// Write a session description, CRLF line endings, fields in RFC 8866 order.
/// Line breaks in text fields become spaces, and a username that is empty or
/// has a space in it is written as "-", so the output always parses. Every
/// media should have an effective connection, as parse() guarantees.
[[nodiscard]] auto to_string(Session const& session) -> std::string;

}  // namespace statusbar::sdp
