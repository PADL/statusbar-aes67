// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/sdp/sdp.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace statusbar::sdp {

namespace {

/// Take the text up to the next `separator` (or all of it) from `rest`.
auto take(std::string_view& rest, char const separator) noexcept -> std::string_view
{
    auto const end = rest.find(separator);
    auto const token = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    return token;
}

template <typename T>
auto parse_number(std::string_view const text) noexcept -> std::optional<T>
{
    T value{};
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

/// A ptime in milliseconds ("1", "0.125", "0.12") as whole microseconds.
auto parse_ptime_us(std::string_view text) noexcept -> std::optional<uint32_t>
{
    auto const whole = parse_number<uint32_t>(take(text, '.'));
    if (!whole || *whole > 1000) {
        return std::nullopt;
    }
    uint32_t micros = *whole * 1000;
    uint32_t scale = 100;
    for (size_t i = 0; i < text.size(); ++i) {
        char const c = text[i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        auto const digit = static_cast<uint32_t>(c - '0');
        if (scale > 0) {
            micros += digit * scale;
            scale /= 10;
        } else if (i == 3 && digit >= 5) {
            ++micros;  // round on the first digit past microseconds
        }
    }
    return micros;
}

/// `IN IP4 <address>` with the address left in `rest`.
auto parse_network(std::string_view& rest) noexcept -> Status
{
    if (take(rest, ' ') != "IN" || take(rest, ' ') != "IP4") {
        return failure(SdpError::unsupported_network);
    }
    return success();
}

/// `<sess-id> <sess-version>` into `session`; false when either is not a number.
[[nodiscard]] auto parse_session_id_and_version(std::string_view& rest, Session& session) noexcept -> bool
{
    auto const id = parse_number<uint64_t>(take(rest, ' '));
    auto const version = parse_number<uint64_t>(take(rest, ' '));
    if (!id || !version) {
        return false;
    }
    session.session_id = *id;
    session.session_version = *version;
    return true;
}

/// `IN IP4 <address>` into `session.origin_address`: an IPv4 literal or a fully qualified domain name (RFC 8866 Section 5.2).
[[nodiscard]] auto parse_origin_address(std::string_view rest, Session& session) -> Status
{
    if (auto const network = parse_network(rest); !network) {
        return network;
    }
    auto const address = take(rest, ' ');
    if (address.empty()) {
        return failure(SdpError::malformed_line);
    }
    session.origin_address = std::string{address};
    return success();
}

/// The `o=` line into `session` (RFC 8866 Section 5.2); `session` may be partly written on failure.
[[nodiscard]] auto parse_origin(std::string_view rest, Session& session) -> Status
{
    session.username = take(rest, ' ');
    if (!parse_session_id_and_version(rest, session)) {
        return failure(SdpError::malformed_line);
    }
    return parse_origin_address(rest, session);
}

/// The `c=` line: `IN IP4 <address>[/<ttl>[/<count>]]`; a count of addresses is not used by AES67 and is ignored.
[[nodiscard]] auto parse_connection(std::string_view rest) noexcept -> StatusValue<Connection>
{
    if (auto const network = parse_network(rest); !network) {
        return std::unexpected(network.error());
    }
    auto const address = ip::parse_ipv4(take(rest, '/'));
    auto const ttl = rest.empty() ? std::optional<uint8_t>{0} : parse_number<uint8_t>(take(rest, '/'));
    if (!address || !ttl) {
        return failure(SdpError::malformed_line);
    }
    return Connection{.address = *address, .ttl = *ttl};
}

/// The `[:<domain>]` ending a PTP reference into `clock`: a bare number or `domain-nmbr=`;
/// a `domain-name=` is ignored.
[[nodiscard]] auto parse_ptp_domain(std::string_view rest, PtpReference clock) -> StatusValue<PtpReference>
{
    if (rest.empty() || rest.starts_with("domain-name=")) {
        return clock;
    }
    if (rest.starts_with("domain-nmbr=")) {
        rest.remove_prefix(std::string_view{"domain-nmbr="}.size());
    }
    auto const domain = parse_number<uint8_t>(rest);
    if (!domain || *domain > 127) {
        return failure(SdpError::malformed_line);
    }
    clock.domain = *domain;
    return clock;
}

/// The `<gmid>|traceable[:<domain>]` after a PTP reference's version into `clock`.
[[nodiscard]] auto parse_ptp_grandmaster(std::string_view rest, PtpReference clock) -> StatusValue<PtpReference>
{
    auto const grandmaster = take(rest, ':');
    if (grandmaster == "traceable") {
        clock.traceable = true;
        return clock;
    }
    clock.grandmaster = ieee::eui64_from_string(grandmaster);
    if (!clock.grandmaster) {
        return failure(SdpError::malformed_line);
    }
    return parse_ptp_domain(rest, clock);
}

/// A `ptp-version` token: RFC 7273's, IEEE1588-2019, and IEEE802.1AS-2020 as IEEE 802.1AS (its unregistered spelling).
[[nodiscard]] auto parse_ptp_version(std::string_view const token) noexcept -> PtpVersion
{
    static constexpr std::array<std::pair<std::string_view, PtpVersion>, 5> VERSIONS{{
        {"IEEE1588-2002", PtpVersion::ieee1588_2002},
        {"IEEE1588-2008", PtpVersion::ieee1588_2008},
        {"IEEE1588-2019", PtpVersion::ieee1588_2019},
        {"IEEE802.1AS-2011", PtpVersion::ieee802_1as},
        {"IEEE802.1AS-2020", PtpVersion::ieee802_1as},
    }};
    auto const* const it = std::ranges::find(VERSIONS, token, &std::pair<std::string_view, PtpVersion>::first);
    return it == VERSIONS.end() ? PtpVersion::other : it->second;
}

/// The text after `ptp=`: `<version>[:<gmid>|:traceable][:<domain>]`; IEEE 802.1AS's domain is 0 whatever is
/// written (AES67-2018 Clause 8.2).
[[nodiscard]] auto parse_ptp_reference(std::string_view rest) -> StatusValue<PtpReference>
{
    PtpReference clock{.version = parse_ptp_version(take(rest, ':'))};
    if (rest.empty()) {
        return clock;
    }
    auto parsed = parse_ptp_grandmaster(rest, clock);
    if (parsed && parsed->version == PtpVersion::ieee802_1as) {
        parsed->domain = 0;
    }
    return parsed;
}

/// Whether PTP versions `a` and `b` can name one clock: PTPv1 only itself, and IEEE 1588-2008, 1588-2019 and 802.1AS
/// one another (one PTPv2 domain across a boundary clock, AES67-2018 Annex D.1); never another token.
[[nodiscard]] auto same_protocol(PtpVersion const a, PtpVersion const b) noexcept -> bool
{
    if (a == PtpVersion::other || b == PtpVersion::other) {
        return false;
    }
    return (a == PtpVersion::ieee1588_2002) == (b == PtpVersion::ieee1588_2002);
}

/// The `[=<offset>]` of a direct media clock: 0 when absent.
[[nodiscard]] auto parse_direct_offset(std::string_view const text) noexcept -> StatusValue<MediaClock>
{
    if (text.empty()) {
        return MediaClock{.direct_offset = 0};
    }
    auto const offset = parse_number<uint64_t>(text);
    if (!offset) {
        return failure(SdpError::malformed_line);
    }
    return MediaClock{.direct_offset = static_cast<uint32_t>(*offset)};
}

/// `direct[=<offset>][ <parameters>]`: a direct clock with its offset (0 if
/// absent); `sender`; any other kind is a media clock that is neither.
[[nodiscard]] auto parse_media_clock(std::string_view rest) noexcept -> StatusValue<MediaClock>
{
    auto clock = take(rest, ' ');
    auto const kind = take(clock, '=');
    if (kind == "sender") {
        return MediaClock::sender();
    }
    if (kind != "direct") {
        return MediaClock{.other = true};
    }
    return parse_direct_offset(clock);
}

/// Takes ` incl IN IP4|*` from `rest`; false for exclusions and for another network or address type
/// (RFC 4570 allows `*` for the address type).
[[nodiscard]] auto take_ipv4_inclusion(std::string_view& rest) noexcept -> bool
{
    while (rest.starts_with(' ')) {
        rest.remove_prefix(1);
    }
    if (take(rest, ' ') != "incl" || take(rest, ' ') != "IN") {
        return false;
    }
    auto const address_type = take(rest, ' ');
    return address_type == "IP4" || address_type == "*";
}

/// A source filter's destination: an empty inner value for `*`; std::nullopt when malformed.
[[nodiscard]] auto parse_filter_destination(std::string_view const text) noexcept -> std::optional<std::optional<ip::IPv4Address>>
{
    if (text == "*") {
        return std::optional<ip::IPv4Address>{};
    }
    auto const address = ip::parse_ipv4(text);
    if (!address) {
        return std::nullopt;
    }
    return address;
}

/// ` incl IN IP4|* <destination>|* <source>...`; nullopt for exclusions and for
/// filters on another network or address type (RFC 4570 allows `*` for both).
[[nodiscard]] auto parse_source_filter(std::string_view rest) noexcept -> StatusValue<std::optional<SourceFilter>>
{
    if (!take_ipv4_inclusion(rest)) {
        return std::optional<SourceFilter>{};
    }
    auto const destination = parse_filter_destination(take(rest, ' '));
    auto const source = ip::parse_ipv4(take(rest, ' '));
    if (!destination || !source) {
        return failure(SdpError::malformed_line);
    }
    return std::optional{SourceFilter{.destination = *destination, .source = *source}};
}

/// `<port>[ IN IP4 <address>]` (RFC 3605 Section 2.1).
auto parse_rtcp(std::string_view rest) noexcept -> StatusValue<RtcpAttribute>
{
    auto const port = parse_number<uint16_t>(take(rest, ' '));
    if (!port || *port == 0) {
        return failure(SdpError::malformed_line);
    }
    RtcpAttribute rtcp{.port = *port, .address = std::nullopt};
    if (!rest.empty()) {
        if (auto network = parse_network(rest); !network) {
            return std::unexpected(network.error());
        }
        rtcp.address = ip::parse_ipv4(take(rest, ' '));
        if (!rtcp.address) {
            return failure(SdpError::malformed_line);
        }
    }
    return rtcp;
}

auto equal_ignoring_case(std::string_view const a, std::string_view const b) noexcept -> bool
{
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

enum class RtpmapResult
{
    other_payload_type,
    applied,
    unsupported,
};

/// An rtpmap encoding name L16, L24, L32 or AM824 (case-insensitive); std::nullopt for any other.
[[nodiscard]] auto parse_encoding(std::string_view const name) noexcept -> std::optional<Encoding>
{
    static constexpr std::array<std::pair<std::string_view, Encoding>, 4> ENCODINGS{{
        {"L16", Encoding::l16},
        {"L24", Encoding::l24},
        {"L32", Encoding::l32},
        {"AM824", Encoding::am824},
    }};
    for (auto const& [text, encoding] : ENCODINGS) {
        if (equal_ignoring_case(name, text)) {
            return encoding;
        }
    }
    return std::nullopt;
}

/// The `<rate>[/<channels>]` ending an rtpmap, as a format; std::nullopt when either is missing or 0.
[[nodiscard]] auto parse_rate_and_channels(std::string_view rest, uint8_t const payload_type, Encoding const encoding) noexcept
    -> std::optional<PayloadFormat>
{
    auto const rate = parse_number<uint32_t>(take(rest, '/'));
    auto const channels = rest.empty() ? std::optional<uint16_t>{1} : parse_number<uint16_t>(rest);
    if (!rate || !channels || *rate == 0 || *channels == 0) {
        return std::nullopt;
    }
    return PayloadFormat{.payload_type = payload_type, .encoding = encoding, .sample_rate = *rate, .channels = *channels};
}

/// `<encoding>/<rate>[/<channels>]` for an offered payload type, read into `format` when the encoding is supported.
[[nodiscard]] auto parse_offered_rtpmap(uint8_t const payload_type, std::string_view rest, PayloadFormat& format) noexcept
    -> StatusValue<RtpmapResult>
{
    auto const encoding = parse_encoding(take(rest, '/'));
    if (!encoding) {
        return RtpmapResult::unsupported;
    }
    auto const parsed = parse_rate_and_channels(rest, payload_type, *encoding);
    if (!parsed) {
        return failure(SdpError::malformed_line);
    }
    format = *parsed;
    return RtpmapResult::applied;
}

/// `<pt> <encoding>/<rate>[/<channels>]`, read into `format` if the payload type is
/// one the m= line offers and the encoding L16, L24, L32 or AM824 (subtype names are
/// case-insensitive).
[[nodiscard]] auto parse_rtpmap(std::string_view rest, std::span<uint8_t const> const offered, PayloadFormat& format) noexcept
    -> StatusValue<RtpmapResult>
{
    auto const payload_type = parse_number<uint8_t>(take(rest, ' '));
    if (!payload_type) {
        return failure(SdpError::malformed_line);
    }
    if (!std::ranges::contains(offered, *payload_type)) {
        return RtpmapResult::other_payload_type;
    }
    return parse_offered_rtpmap(*payload_type, rest, format);
}

auto parse_direction(std::string_view const name) noexcept -> std::optional<Direction>
{
    if (name == "sendrecv") {
        return Direction::sendrecv;
    }
    if (name == "sendonly") {
        return Direction::sendonly;
    }
    if (name == "recvonly") {
        return Direction::recvonly;
    }
    if (name == "inactive") {
        return Direction::inactive;
    }
    return std::nullopt;
}

class Parser
{
  public:
    auto line(char const type, std::string_view const value) -> Status
    {
        if (!seen_version_) {
            if (type != 'v') {
                return failure(SdpError::missing_field);
            }
            seen_version_ = true;
            return value == "0" ? success() : failure(SdpError::unsupported_version);
        }
        switch (type) {
            case 'o':
                seen_origin_ = true;
                return parse_origin(value, session_);
            case 's':
                seen_name_ = true;
                session_.name = value;
                return success();
            case 't':
                seen_timing_ = true;
                return success();
            case 'i':
                if (!skipping_) {
                    (media_ != nullptr ? media_->information : session_.information) = value;
                }
                return success();
            case 'c':
                return connection(value);
            case 'm':
                return media(value);
            case 'a':
                return attribute(value);
            default:
                return success();  // b=, k=, r=, z=, ...: nothing the bridge needs
        }
    }

    auto finish() -> StatusValue<Session>
    {
        if (auto const done = end_media(); !done) {
            return std::unexpected(done.error());
        }
        if (!seen_version_ || !seen_origin_ || !seen_name_ || !seen_timing_) {
            return failure(SdpError::missing_field);
        }
        if (session_.media.empty() && unsupported_media_) {
            return failure(SdpError::unsupported_encoding);
        }
        for (auto const& m : session_.media) {
            if (!session_.effective_connection(m)) {
                return failure(SdpError::missing_field);
            }
        }
        return std::move(session_);
    }

  private:
    auto connection(std::string_view const value) -> Status
    {
        if (skipping_) {
            return success();
        }
        auto parsed = parse_connection(value);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        (media_ != nullptr ? media_->connection : session_.connection) = *parsed;
        return success();
    }

    /// The `m=` line: closes the previous media, and opens an audio one or skips any other.
    auto media(std::string_view rest) -> Status
    {
        if (auto const done = end_media(); !done) {
            return done;
        }
        auto const kind = take(rest, ' ');
        auto const port = parse_number<uint16_t>(take_port(rest));
        auto const proto = take(rest, ' ');
        skipping_ = kind != "audio" || proto != "RTP/AVP";
        return skipping_ ? success() : begin_audio_media(port, rest);
    }

    /// The port of an `m=` line taken from `rest`; a "/<count>" of ports is ignored.
    [[nodiscard]] static auto take_port(std::string_view& rest) noexcept -> std::string_view
    {
        auto port_field = take(rest, ' ');
        return take(port_field, '/');
    }

    /// Opens an audio media section offering the payload types listed in `rest`.
    auto begin_audio_media(std::optional<uint16_t> const port, std::string_view const rest) -> Status
    {
        if (!parse_offered(rest) || !port || offered_.empty()) {
            return failure(SdpError::malformed_line);
        }
        media_ = &session_.media.emplace_back(Media{.port = *port, .payload_type = offered_.front()});
        rtpmap_ = RtpmapResult::other_payload_type;
        formats_.clear();
        return success();
    }

    /// The payload types an `m=` line offers, in order of preference (RFC 8866 Section 5.14), into `offered_`;
    /// false when one is not a number up to 127.
    [[nodiscard]] auto parse_offered(std::string_view rest) -> bool
    {
        offered_.clear();
        while (!rest.empty()) {
            auto const payload_type = parse_number<uint8_t>(take(rest, ' '));
            if (!payload_type || *payload_type > 127) {
                return false;
            }
            offered_.push_back(*payload_type);
        }
        return true;
    }

    /// Close the current audio media: kept with a supported rtpmap, dropped if it offers only other
    /// encodings or only static payload types (RFC 3551 §6), an error for a dynamic one without rtpmap.
    auto end_media() -> Status
    {
        auto* const closing = std::exchange(media_, nullptr);
        if (closing == nullptr) {
            return success();
        }
        if (!formats_.empty()) {
            // RFC 8866 §5.14: the m= line's order is the order of preference
            auto const rank = [this](PayloadFormat const& f) {
                return std::ranges::find(offered_, f.payload_type) - offered_.begin();
            };
            std::ranges::stable_sort(formats_, {}, rank);
            closing->set_format(formats_.front());
            for (auto const& f : std::span{formats_}.subspan(1)) {
                (void)closing->add_alternative(f);  // beyond the capacity, ignored
            }
            return success();
        }
        bool const all_static = std::ranges::all_of(offered_, [](uint8_t const pt) { return pt < 96; });
        if (rtpmap_ == RtpmapResult::unsupported || all_static) {
            unsupported_media_ = true;
            session_.media.pop_back();
            return success();
        }
        return failure(SdpError::missing_field);
    }

    /// An `a=` line's handler, and whether it applies only within a media section.
    struct AttributeHandler
    {
        std::string_view name;
        bool media_only;
        auto (Parser::*parse)(std::string_view) -> Status;
    };
    static std::array<AttributeHandler, 8> const ATTRIBUTES;

    /// An `a=` line: a direction, or one of ATTRIBUTES; any other is ignored.
    auto attribute(std::string_view rest) -> Status
    {
        if (skipping_) {
            return success();
        }
        auto const name = take(rest, ':');
        if (auto const direction = parse_direction(name)) {
            (media_ != nullptr ? media_->direction : session_.direction) = direction;
            return success();
        }
        auto const* const handler = std::ranges::find(ATTRIBUTES, name, &AttributeHandler::name);
        if (handler == ATTRIBUTES.end() || (handler->media_only && media_ == nullptr)) {
            return success();
        }
        return (this->*handler->parse)(rest);
    }

    /// `a=ts-refclk:`: every PTP reference kept, the level noting any other source (RFC 7273 Section 4.8).
    auto ts_refclk_attribute(std::string_view const rest) -> Status
    {
        auto& clocks = media_ != nullptr ? media_->clocks : session_.clocks;
        if (!rest.starts_with("ptp=")) {
            clocks.other = true;
            return success();
        }
        auto clock = parse_ptp_reference(rest.substr(4));
        if (!clock) {
            return std::unexpected(clock.error());
        }
        (void)clocks.ptp.try_push_back(*clock);  // beyond ReferenceClocks::MAX, ignored
        return success();
    }

    /// `a=mediaclk:`; a level that names both a direct clock and another keeps the direct one.
    auto mediaclk_attribute(std::string_view const rest) -> Status
    {
        auto const clock = parse_media_clock(rest);
        if (!clock) {
            return std::unexpected(clock.error());
        }
        auto& slot = media_ != nullptr ? media_->media_clock : session_.media_clock;
        if (!slot || !slot->direct_offset || clock->direct_offset) {
            slot = *clock;
        }
        return success();
    }

    /// `a=clock-domain:PTPv2 <domain>`; another clock kind, or a domain out of range, is ignored.
    auto clock_domain_attribute(std::string_view const rest) -> Status
    {
        if (!rest.starts_with("PTPv2 ")) {
            return success();
        }
        auto const domain = parse_number<uint8_t>(rest.substr(6));
        if (domain && *domain <= 127) {
            (media_ != nullptr ? media_->clock_domain : session_.clock_domain) = domain;
        }
        return success();
    }

    /// `a=source-filter:` (RFC 4570); exclusions and other address types are ignored.
    auto source_filter_attribute(std::string_view const rest) -> Status
    {
        auto const filter = parse_source_filter(rest);
        if (!filter) {
            return std::unexpected(filter.error());
        }
        if (*filter) {
            (media_ != nullptr ? media_->source_filter : session_.source_filter) = *filter;
        }
        return success();
    }

    /// `a=rtpmap:`; every offered payload type with a usable rtpmap is kept (the first map of each).
    auto rtpmap_attribute(std::string_view const rest) -> Status
    {
        PayloadFormat format;
        auto const result = parse_rtpmap(rest, offered_, format);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == RtpmapResult::applied) {
            add_format(format);
        } else if (*result == RtpmapResult::unsupported) {
            rtpmap_ = *result;
        }
        return success();
    }

    /// Keeps `format` unless its payload type already has one.
    void add_format(PayloadFormat const& format)
    {
        if (std::ranges::none_of(formats_, [&](PayloadFormat const& f) { return f.payload_type == format.payload_type; })) {
            formats_.push_back(format);
        }
    }

    /// `a=rtcp:` (RFC 3605).
    auto rtcp_attribute(std::string_view const rest) -> Status
    {
        auto rtcp = parse_rtcp(rest);
        if (!rtcp) {
            return std::unexpected(rtcp.error());
        }
        media_->rtcp = *rtcp;
        return success();
    }

    /// `a=rtcp-mux` (RFC 5761).
    auto rtcp_mux_attribute(std::string_view /*rest*/) -> Status
    {
        media_->rtcp_mux = true;
        return success();
    }

    /// `a=ptime:` in milliseconds; 0 is refused.
    auto ptime_attribute(std::string_view const rest) -> Status
    {
        auto const ptime = parse_ptime_us(rest);
        if (!ptime || *ptime == 0) {
            return failure(SdpError::malformed_line);
        }
        media_->packet_time_us = *ptime;
        return success();
    }

    Session session_;
    Media* media_{nullptr};
    std::vector<uint8_t> offered_;                           ///< the current media's payload types, from its m= line
    RtpmapResult rtpmap_{RtpmapResult::other_payload_type};  ///< unsupported once an offered map is unusable
    std::vector<PayloadFormat> formats_;                     ///< the current media's usable maps
    bool seen_version_{false};
    bool seen_origin_{false};
    bool seen_name_{false};
    bool seen_timing_{false};
    bool skipping_{false};
    bool unsupported_media_{false};
};

std::array<Parser::AttributeHandler, 8> const Parser::ATTRIBUTES{{
    {.name = "ts-refclk", .media_only = false, .parse = &Parser::ts_refclk_attribute},
    {.name = "mediaclk", .media_only = false, .parse = &Parser::mediaclk_attribute},
    {.name = "clock-domain", .media_only = false, .parse = &Parser::clock_domain_attribute},
    {.name = "source-filter", .media_only = false, .parse = &Parser::source_filter_attribute},
    {.name = "rtpmap", .media_only = true, .parse = &Parser::rtpmap_attribute},
    {.name = "rtcp", .media_only = true, .parse = &Parser::rtcp_attribute},
    {.name = "rtcp-mux", .media_only = true, .parse = &Parser::rtcp_mux_attribute},
    {.name = "ptime", .media_only = true, .parse = &Parser::ptime_attribute},
}};

}  // namespace

auto PtpReference::same_clock(PtpReference const& other) const noexcept -> bool
{
    return grandmaster && other.grandmaster && *grandmaster == *other.grandmaster && domain == other.domain &&
        same_protocol(version, other.version);
}

auto parse(std::string_view text) -> StatusValue<Session>
{
    Parser parser;
    while (!text.empty()) {
        auto line = take(text, '\n');
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (line.size() < 2 || line[1] != '=') {
            return failure(SdpError::malformed_line);
        }
        if (auto const status = parser.line(line[0], line.substr(2)); !status) {
            return std::unexpected(status.error());
        }
    }
    return parser.finish();
}

auto parse_origin_line(std::string_view const value, Session& session) -> Status
{
    Session parsed = session;
    if (auto const status = parse_origin(value, parsed); !status) {
        return status;  // `session` untouched
    }
    session = std::move(parsed);
    return success();
}

}  // namespace statusbar::sdp
