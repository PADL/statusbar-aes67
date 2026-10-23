#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// SDP - RFC 8866 session descriptions, for AES67 audio streams.
///
/// Parses and writes the subset AES67 uses: an origin, a session name, IPv4
/// connection data (multicast TTL included), and audio media over RTP/AVP with
/// L16/L24 rtpmaps, ptime, the RFC 7273 reference clock and media clock
/// (`ts-refclk:ptp=...`, `mediaclk:direct=<offset>`), RAVENNA's
/// `clock-domain:PTPv2 <n>`, and RFC 4570 source
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

/// The encodings carried: AES67 linear PCM (RFC 3551 L16, RFC 3190 L24) and
/// RAVENNA's AES3 transport, AM824 (IEC 61883-6 32-bit subframes, as ST 2110-31).
enum class Encoding : uint8_t
{
    l16,
    l24,
    am824,
};

[[nodiscard]] constexpr auto bytes_per_sample(Encoding const encoding) noexcept -> uint32_t
{
    switch (encoding) {
        case Encoding::l16:
            return 2;
        case Encoding::l24:
            return 3;
        case Encoding::am824:
            return 4;
    }
    return 0;
}

/// The rtpmap encoding name
[[nodiscard]] constexpr auto encoding_name(Encoding const encoding) noexcept -> char const*
{
    switch (encoding) {
        case Encoding::l16:
            return "L16";
        case Encoding::l24:
            return "L24";
        case Encoding::am824:
            return "AM824";
    }
    return "";
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

/// `a=ts-refclk:ptp=<version>:<grandmaster>[:<domain>]` or `ptp=<version>:traceable`
/// (RFC 7273 section 4.8). The domain may be written bare (`:0`, as AES67 devices
/// do), as `domain-nmbr=0`, or left out (domain 0); to_string() always writes it
/// bare, 0 included; a 2002 `domain-name=` is read as domain 0. RFC 7273
/// requires a grandmaster or `traceable`; parse() tolerates a bare version (the
/// grandmaster stays nullopt), to_string() writes nothing for one.
struct PtpReference
{
    std::string version{"IEEE1588-2008"};    ///< a sender writes PTPv2; IEEE802.1AS-2011 etc. are read
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

/// `a=mediaclk:` (RFC 7273 section 5): `direct[=<offset>]` is the PTP clock itself,
/// offset by `<offset>` (0 when absent); any other kind (`sender`, `IEEE1722`) is a
/// clock the bridge cannot derive from PTP. A media-level clock of either kind
/// overrides a session-level one; a level naming both keeps the direct one.
struct MediaClock
{
    std::optional<uint32_t> direct_offset;  ///< nullopt for a media clock that is not direct

    auto operator==(MediaClock const&) const noexcept -> bool = default;
};

enum class Direction : uint8_t
{
    sendrecv,
    sendonly,
    recvonly,
    inactive,
};

/// One `m=audio <port> RTP/AVP <pt>...` section
struct Media
{
    uint16_t port{0};
    uint8_t payload_type{0};  ///< the first on the m= line with an L16 or L24 rtpmap
    Encoding encoding{Encoding::l24};
    uint32_t sample_rate{48000};
    uint16_t channels{1};
    uint32_t packet_time_us{1000};  ///< a=ptime; AES67's default is 1 ms
    std::string information;        ///< i=
    std::optional<Direction> direction;
    std::optional<Connection> connection;
    std::optional<PtpReference> clock;
    std::optional<MediaClock> media_clock;
    std::optional<SourceFilter> source_filter;
    std::optional<uint8_t> clock_domain;  ///< a=clock-domain (see ClockDomain below)

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
    std::string origin_address{"0.0.0.0"};  ///< o= unicast-address: an IPv4 literal or a host name
    std::string name{"-"};                  ///< s=; RFC 8866 requires one, "-" when there is nothing to say
    std::string information;
    std::optional<Direction> direction;
    std::optional<Connection> connection;
    std::optional<PtpReference> clock;
    std::optional<MediaClock> media_clock;
    std::optional<SourceFilter> source_filter;
    std::optional<uint8_t> clock_domain;  ///< a=clock-domain (see effective_clock_domain())
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
    /// `a=clock-domain:PTPv2 <domain>` in force for `m` (media level over
    /// session level): the PTPv2 domain of the reference clock, which RAVENNA
    /// and Dante write beside ts-refclk and some receivers look for. Other clock
    /// kinds are not kept. It duplicates PtpReference::domain; writers set both.
    [[nodiscard]] auto effective_clock_domain(Media const& m) const noexcept -> std::optional<uint8_t>
    {
        return m.clock_domain ? m.clock_domain : clock_domain;
    }
    /// The direct media clock offset in force for `m`, or nullopt when no media
    /// clock was given or the one in force (media level over session level) is
    /// not direct: the stream's timestamps are then not PTP-locked.
    [[nodiscard]] auto effective_media_clock_offset(Media const& m) const noexcept -> std::optional<uint32_t>
    {
        auto const& clock = m.media_clock ? m.media_clock : media_clock;
        return clock ? clock->direct_offset : std::nullopt;
    }

    auto operator==(Session const&) const -> bool = default;
};

/// Parse a session description. Lines may end in CRLF or LF. Fails unless it
/// has v=0, an IN IP4 origin, s= and t=, and every audio media kept has a
/// connection at media or session level. An audio media is kept with the first
/// payload type on its m= line that has an L16 or L24 rtpmap; one offering only
/// other encodings, or only static payload types without an rtpmap, is skipped,
/// and if that leaves none the error is unsupported_encoding. A dynamic payload
/// type without an rtpmap is missing_field.
[[nodiscard]] auto parse(std::string_view text) -> StatusValue<Session>;

/// Read the value of an `o=` line ("<username> <sess-id> <sess-version> IN
/// IP4 <address>") into `session`'s origin fields, as parse() does: for a SAP
/// deletion, which may carry only that line. On failure `session` is unchanged.
[[nodiscard]] auto parse_origin_line(std::string_view value, Session& session) -> Status;

/// Write a session description, CRLF line endings, fields in RFC 8866 order.
/// Line breaks in text fields become spaces, a username that is empty or has a
/// space in it is written as "-", and an origin address that is not one token
/// as 0.0.0.0, so the output always parses. A PtpReference with neither
/// grandmaster nor traceable is not written, and a unicast connection's TTL is
/// dropped, as RFC 7273 and RFC 8866 require. Every media should have an
/// effective connection, as parse() guarantees.
[[nodiscard]] auto to_string(Session const& session) -> std::string;

}  // namespace statusbar::sdp
