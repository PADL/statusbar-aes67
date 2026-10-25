#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// SDP - RFC 8866 session descriptions, the subset AES67 uses: origin, name, IPv4 connection,
/// L16/L24/L32/AM824 audio media, ptime, RFC 7273 clocks, RAVENNA clock-domain, RFC 4570 source
/// filters and the RFC 3605 RTCP port. Session- or media-level values resolve through the effective_*() accessors.

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/sdp/sdp_error.hpp"
#include "statusbar/status/status.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::sdp {

/// The encodings carried: AES67 linear PCM (RFC 3551 L16, RFC 3190 L24), L32
/// (32-bit big-endian PCM, a RAVENNA extension: AES67-2018 mandates only L16/L24)
/// and RAVENNA's AES3 transport, AM824 (IEC 61883-6 32-bit subframes, as ST 2110-31).
enum class Encoding : uint8_t
{
    l16,
    l24,
    am824,
    l32,
};

[[nodiscard]] constexpr auto bytes_per_sample(Encoding const encoding) noexcept -> uint32_t
{
    switch (encoding) {
        case Encoding::l16:
            return 2;
        case Encoding::l24:
            return 3;
        case Encoding::am824:
        case Encoding::l32:
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
        case Encoding::l32:
            return "L32";
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

/// `a=ts-refclk:ptp=<version>:<grandmaster>[:<domain>]` or `ptp=<version>:traceable` (RFC 7273 §4.8).
/// The domain is read bare, as `domain-nmbr=`, or absent (0), and written bare; a bare version (no
/// grandmaster) is read but not written.
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

/// The kinds of `a=mediaclk:` told apart (RFC 7273 §5).
enum class MediaClockKind : uint8_t
{
    direct,  ///< `direct[=<offset>]`: the reference clock itself, offset
    sender,  ///< `sender`: the sender's own clock, not derived from the reference
    other,   ///< any other kind (`IEEE1722=<stream id>`, ...): read, written as `sender`
};

/// `a=mediaclk:` (RFC 7273 §5): `direct[=<offset>]` is the PTP clock itself, offset by `<offset>`
/// (0 when absent); any other kind is a clock not derived from PTP. Media level overrides session.
struct MediaClock
{
    std::optional<uint32_t> direct_offset;  ///< nullopt for a media clock that is not direct
    bool other{false};                      ///< not direct: a kind other than `sender`

    /// `direct=<offset>`.
    [[nodiscard]] static constexpr auto direct(uint32_t const offset = 0) noexcept -> MediaClock
    {
        return MediaClock{.direct_offset = offset};
    }
    /// `sender`.
    [[nodiscard]] static constexpr auto sender() noexcept -> MediaClock { return MediaClock{}; }

    [[nodiscard]] constexpr auto kind() const noexcept -> MediaClockKind
    {
        return direct_offset ? MediaClockKind::direct : other ? MediaClockKind::other : MediaClockKind::sender;
    }

    auto operator==(MediaClock const&) const noexcept -> bool = default;
};

enum class Direction : uint8_t
{
    sendrecv,
    sendonly,
    recvonly,
    inactive,
};

/// `a=rtcp:<port>[ IN IP4 <address>]` (RFC 3605), media level: where the stream's RTCP goes when
/// not to the media's address at its port + 1.
struct RtcpAttribute
{
    uint16_t port{0};
    std::optional<ip::IPv4Address> address;  ///< nullopt: the media's connection address

    auto operator==(RtcpAttribute const&) const noexcept -> bool = default;
};

/// One payload type's rtpmap: `a=rtpmap:<pt> <encoding>/<rate>/<channels>`
struct PayloadFormat
{
    uint8_t payload_type{0};
    Encoding encoding{Encoding::l24};
    uint32_t sample_rate{48000};
    uint16_t channels{1};

    auto operator==(PayloadFormat const&) const noexcept -> bool = default;
};

/// One `m=audio <port> RTP/AVP <pt>...` section
struct Media
{
    /// Supported formats a media may offer beside its primary one.
    static constexpr size_t MAX_ALTERNATIVES = 3;

    uint16_t port{0};
    uint8_t payload_type{0};  ///< the primary format: the first on the m= line with a supported rtpmap
    Encoding encoding{Encoding::l24};
    uint32_t sample_rate{48000};
    uint16_t channels{1};
    std::array<PayloadFormat, MAX_ALTERNATIVES> alternatives{};  ///< further offered formats, in m= line order
    uint8_t alternative_count{0};
    uint32_t packet_time_us{1000};  ///< a=ptime; AES67's default is 1 ms
    std::string information;        ///< i=
    std::optional<Direction> direction;
    std::optional<Connection> connection;
    std::optional<PtpReference> clock;
    std::optional<MediaClock> media_clock;
    std::optional<SourceFilter> source_filter;
    std::optional<uint8_t> clock_domain;  ///< a=clock-domain (see ClockDomain below)
    std::optional<RtcpAttribute> rtcp;    ///< a=rtcp
    bool rtcp_mux{false};                 ///< a=rtcp-mux (RFC 5761) was given; read, never written

    /// The primary format.
    [[nodiscard]] constexpr auto format() const noexcept -> PayloadFormat
    {
        return {.payload_type = payload_type, .encoding = encoding, .sample_rate = sample_rate, .channels = channels};
    }

    /// Make `f` the primary format (its alternatives are unchanged).
    constexpr void set_format(PayloadFormat const& f) noexcept
    {
        payload_type = f.payload_type;
        encoding = f.encoding;
        sample_rate = f.sample_rate;
        channels = f.channels;
    }

    /// The offered formats other than the primary one.
    [[nodiscard]] constexpr auto other_formats() const noexcept -> std::span<PayloadFormat const>
    {
        return std::span{alternatives}.first(std::min<size_t>(alternative_count, MAX_ALTERNATIVES));
    }

    /// Add a format offered beside the primary one; false if there is no room.
    [[nodiscard]] constexpr auto add_alternative(PayloadFormat const& f) noexcept -> bool
    {
        if (alternative_count >= MAX_ALTERNATIVES) {
            return false;
        }
        alternatives[alternative_count++] = f;
        return true;
    }

    /// Make the first offered format with `encoding` the primary one, swapping it
    /// with the old primary; false, and unchanged, if none is offered.
    [[nodiscard]] constexpr auto prefer(Encoding const wanted) noexcept -> bool
    {
        if (encoding == wanted) {
            return true;
        }
        for (auto& alternative : std::span{alternatives}.first(other_formats().size())) {
            if (alternative.encoding == wanted) {
                auto const primary = format();
                set_format(alternative);
                alternative = primary;
                return true;
            }
        }
        return false;
    }

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
    /// `a=clock-domain:PTPv2 <domain>` in force for `m` (media level over session level), as
    /// RAVENNA and Dante write beside ts-refclk; it duplicates PtpReference::domain, and writers set both.
    [[nodiscard]] auto effective_clock_domain(Media const& m) const noexcept -> std::optional<uint8_t>
    {
        return m.clock_domain ? m.clock_domain : clock_domain;
    }
    /// The media clock in force for `m` (media level over session level), nullopt when none is given.
    [[nodiscard]] auto effective_media_clock(Media const& m) const noexcept -> std::optional<MediaClock>
    {
        return m.media_clock ? m.media_clock : media_clock;
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

/// Parse a session description (CRLF or LF). Fails without v=0, an IN IP4 origin, s=, t= and a
/// connection for every audio media kept; unsupported_encoding when none is kept, missing_field for
/// a dynamic payload type without an rtpmap. The primary format is the first with a supported rtpmap.
[[nodiscard]] auto parse(std::string_view text) -> StatusValue<Session>;

/// Read the value of an `o=` line ("<username> <sess-id> <sess-version> IN
/// IP4 <address>") into `session`'s origin fields, as parse() does: for a SAP
/// deletion, which may carry only that line. On failure `session` is unchanged.
[[nodiscard]] auto parse_origin_line(std::string_view value, Session& session) -> Status;

/// The text an `a=ts-refclk:` line carries for `clock` ("ptp=IEEE1588-2008:<EUI-64>:<domain>" or
/// "ptp=<version>:traceable"), as to_string() writes it; empty when it would not be written.
[[nodiscard]] auto ts_refclk_value(PtpReference const& clock) -> std::string;

/// The text an `a=mediaclk:` line carries for `clock` ("direct=<offset>", else "sender").
[[nodiscard]] auto media_clock_value(MediaClock const& clock) -> std::string;

/// Write a session description, CRLF line endings, fields in RFC 8866 order, sanitised so the
/// output always parses. A PtpReference with neither grandmaster nor traceable, and a unicast
/// connection's TTL, are not written. Every media should have an effective connection.
[[nodiscard]] auto to_string(Session const& session) -> std::string;

}  // namespace statusbar::sdp
