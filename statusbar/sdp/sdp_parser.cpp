// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/sdp/sdp.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

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

auto parse_ipv4(std::string_view text) noexcept -> std::optional<ip::IPv4Address>
{
    std::array<uint8_t, 4> octets{};
    for (size_t i = 0; i < octets.size(); ++i) {
        auto const octet = parse_number<uint8_t>(take(text, '.'));
        if (!octet) {
            return std::nullopt;
        }
        octets[i] = *octet;
    }
    if (!text.empty()) {
        return std::nullopt;
    }
    return ip::IPv4Address{octets[0], octets[1], octets[2], octets[3]};
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

auto parse_origin(std::string_view rest, Session& session) -> Status
{
    session.username = take(rest, ' ');
    auto const id = parse_number<uint64_t>(take(rest, ' '));
    auto const version = parse_number<uint64_t>(take(rest, ' '));
    if (!id || !version) {
        return failure(SdpError::malformed_line);
    }
    if (auto const network = parse_network(rest); !network) {
        return network;
    }
    auto const address = parse_ipv4(take(rest, ' '));
    if (!address) {
        return failure(SdpError::malformed_line);
    }
    session.session_id = *id;
    session.session_version = *version;
    session.origin = *address;
    return success();
}

auto parse_connection(std::string_view rest) noexcept -> StatusValue<Connection>
{
    if (auto const network = parse_network(rest); !network) {
        return std::unexpected(network.error());
    }
    Connection connection{};
    auto const address = parse_ipv4(take(rest, '/'));
    if (!address) {
        return failure(SdpError::malformed_line);
    }
    connection.address = *address;
    if (!rest.empty()) {
        // "/<ttl>[/<count>]": a count of addresses is not used by AES67 and is ignored.
        auto const ttl = parse_number<uint8_t>(take(rest, '/'));
        if (!ttl) {
            return failure(SdpError::malformed_line);
        }
        connection.ttl = *ttl;
    }
    return connection;
}

/// The text after `ptp=`: `<version>[:<gmid>|:traceable][:<domain>]`.
auto parse_ptp_reference(std::string_view rest) -> StatusValue<PtpReference>
{
    PtpReference clock{};
    clock.version = take(rest, ':');
    if (rest.empty()) {
        return clock;
    }
    auto const grandmaster = take(rest, ':');
    if (grandmaster == "traceable") {
        clock.traceable = true;
        return clock;
    }
    clock.grandmaster = ieee::eui64_from_string(grandmaster);
    if (!clock.grandmaster) {
        return failure(SdpError::malformed_line);
    }
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

/// `direct[=<offset>][ <parameters>]`: the offset (0 if absent), or nullopt for
/// another kind of media clock.
auto parse_media_clock(std::string_view rest) noexcept -> StatusValue<std::optional<uint32_t>>
{
    auto clock = take(rest, ' ');
    if (take(clock, '=') != "direct") {
        return std::optional<uint32_t>{};
    }
    if (clock.empty()) {
        return std::optional<uint32_t>{0};
    }
    auto const offset = parse_number<uint64_t>(clock);
    if (!offset) {
        return failure(SdpError::malformed_line);
    }
    return std::optional{static_cast<uint32_t>(*offset)};
}

/// ` incl IN IP4 <destination>|* <source>...`; nullopt for exclusions.
auto parse_source_filter(std::string_view rest) noexcept -> StatusValue<std::optional<SourceFilter>>
{
    while (rest.starts_with(' ')) {
        rest.remove_prefix(1);
    }
    if (take(rest, ' ') != "incl") {
        return std::optional<SourceFilter>{};
    }
    if (auto const network = parse_network(rest); !network) {
        return std::unexpected(network.error());
    }
    SourceFilter filter{};
    if (auto const destination = take(rest, ' '); destination != "*") {
        filter.destination = parse_ipv4(destination);
        if (!filter.destination) {
            return failure(SdpError::malformed_line);
        }
    }
    auto const source = parse_ipv4(take(rest, ' '));
    if (!source) {
        return failure(SdpError::malformed_line);
    }
    filter.source = *source;
    return std::optional{filter};
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

/// `<pt> <encoding>/<rate>[/<channels>]`, applied to `media` if the payload type
/// is its own and the encoding L16 or L24 (subtype names are case-insensitive).
auto parse_rtpmap(std::string_view rest, Media& media) noexcept -> StatusValue<RtpmapResult>
{
    auto const payload_type = parse_number<uint8_t>(take(rest, ' '));
    if (!payload_type) {
        return failure(SdpError::malformed_line);
    }
    if (*payload_type != media.payload_type) {
        return RtpmapResult::other_payload_type;
    }
    auto const encoding = take(rest, '/');
    if (equal_ignoring_case(encoding, "L16")) {
        media.encoding = Encoding::l16;
    } else if (equal_ignoring_case(encoding, "L24")) {
        media.encoding = Encoding::l24;
    } else {
        return RtpmapResult::unsupported;
    }
    auto const rate = parse_number<uint32_t>(take(rest, '/'));
    auto const channels = rest.empty() ? std::optional<uint16_t>{1} : parse_number<uint16_t>(rest);
    if (!rate || !channels || *rate == 0 || *channels == 0) {
        return failure(SdpError::malformed_line);
    }
    media.sample_rate = *rate;
    media.channels = *channels;
    return RtpmapResult::applied;
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

    auto media(std::string_view rest) -> Status
    {
        if (auto const done = end_media(); !done) {
            return done;
        }
        auto const kind = take(rest, ' ');
        auto port_field = take(rest, ' ');
        auto const port = parse_number<uint16_t>(take(port_field, '/'));  // a "/<count>" of ports is ignored
        auto const proto = take(rest, ' ');
        auto const payload_type = parse_number<uint8_t>(take(rest, ' '));
        if (kind != "audio" || proto != "RTP/AVP") {
            skipping_ = true;
            return success();
        }
        if (!port || !payload_type || *payload_type > 127) {
            return failure(SdpError::malformed_line);
        }
        skipping_ = false;
        media_ = &session_.media.emplace_back(Media{.port = *port, .payload_type = *payload_type});
        rtpmap_ = RtpmapResult::other_payload_type;
        return success();
    }

    /// Close the current audio media: an error if it had no rtpmap for its payload
    /// type, dropped if the rtpmap was an encoding the bridge does not carry.
    auto end_media() -> Status
    {
        auto const* const closing = std::exchange(media_, nullptr);
        if (closing == nullptr) {
            return success();
        }
        switch (rtpmap_) {
            case RtpmapResult::applied:
                return success();
            case RtpmapResult::unsupported:
                unsupported_media_ = true;
                session_.media.pop_back();
                return success();
            case RtpmapResult::other_payload_type:
                break;
        }
        return failure(SdpError::missing_field);
    }

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
        if (name == "ts-refclk") {
            // RFC 7273 allows several reference clocks; keep the PTP one.
            if (!rest.starts_with("ptp=")) {
                return success();
            }
            auto clock = parse_ptp_reference(rest.substr(4));
            if (!clock) {
                return std::unexpected(clock.error());
            }
            (media_ != nullptr ? media_->clock : session_.clock) = std::move(*clock);
            return success();
        }
        if (name == "mediaclk") {
            auto const offset = parse_media_clock(rest);
            if (!offset) {
                return std::unexpected(offset.error());
            }
            if (*offset) {
                (media_ != nullptr ? media_->media_clock_offset : session_.media_clock_offset) = *offset;
            }
            return success();
        }
        if (name == "source-filter") {
            auto const filter = parse_source_filter(rest);
            if (!filter) {
                return std::unexpected(filter.error());
            }
            if (*filter) {
                (media_ != nullptr ? media_->source_filter : session_.source_filter) = *filter;
            }
            return success();
        }
        if (media_ == nullptr) {
            return success();
        }
        if (name == "rtpmap") {
            auto const result = parse_rtpmap(rest, *media_);
            if (!result) {
                return std::unexpected(result.error());
            }
            if (*result != RtpmapResult::other_payload_type) {
                rtpmap_ = *result;
            }
            return success();
        }
        if (name == "ptime") {
            auto const ptime = parse_ptime_us(rest);
            if (!ptime || *ptime == 0) {
                return failure(SdpError::malformed_line);
            }
            media_->packet_time_us = *ptime;
            return success();
        }
        return success();
    }

    Session session_;
    Media* media_{nullptr};
    RtpmapResult rtpmap_{RtpmapResult::other_payload_type};
    bool seen_version_{false};
    bool seen_origin_{false};
    bool seen_name_{false};
    bool seen_timing_{false};
    bool skipping_{false};
    bool unsupported_media_{false};
};

}  // namespace

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

}  // namespace statusbar::sdp
