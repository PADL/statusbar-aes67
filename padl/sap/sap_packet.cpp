// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/sap/sap_packet.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <string>

namespace padl::sap {

namespace {

/// A version 1 header with no IPv6 origin, encryption or compression.
[[nodiscard]] auto is_supported_header(SapHeader const& header) noexcept -> bool
{
    return header.version() == SapHeader::VERSION && !header.ipv6() && !header.encrypted() && !header.compressed();
}

/// The message a supported header at the start of `packet` begins: the offset past its authentication data, or
/// nullopt.
[[nodiscard]] auto parse_header(std::span<uint8_t const> const packet, SapMessage& message) noexcept -> std::optional<size_t>
{
    SapHeader header{};
    statusbar::BufferDeserializerBuilder reader{packet};
    reader.parse(&header);
    if (!reader || !is_supported_header(header) || !reader.skip(size_t{header.auth_length.get()} * 4)) {
        return std::nullopt;
    }
    message = SapMessage{
        .deletion = header.deletion(),
        .message_id_hash = header.message_id_hash.get(),
        .origin = statusbar::ip::IPv4Address{header.origin.get()},
    };
    return reader.position();
}

/// The optional payload type and the SDP text from `rest`: SDP text starts "v=0" (a deletion's may be only its "o="
/// line, or nothing), anything else is a NUL-terminated type. False when the NUL is missing.
[[nodiscard]] auto parse_payload(std::string_view rest, SapMessage& message) -> bool
{
    if (!rest.empty() && !rest.starts_with("v=") && !rest.starts_with("o=")) {
        auto const nul = rest.find('\0');
        if (nul == std::string_view::npos) {
            return false;
        }
        message.payload_type = std::string{rest.substr(0, nul)};
        rest.remove_prefix(nul + 1);
    }
    message.sdp = rest;
    return true;
}

}  // namespace

auto serialize_sap_packet(SapHeader const& header, std::string_view const sdp, std::span<uint8_t> const datagram) noexcept
    -> statusbar::StatusValue<size_t>
{
    SapHeader fixed = header;
    fixed.auth_length = 0U;
    statusbar::BufferSerializerBuilderWithBuffer writer{datagram};
    writer.append(fixed).append(statusbar::make_const_span(SAP_PAYLOAD_TYPE)).append(octet_t{0U});
    if (!writer.append(statusbar::make_const_span(sdp))) {
        return statusbar::failure(writer.error());
    }
    return writer.position();
}

auto parse_sap_packet(std::span<uint8_t const> const packet) -> std::optional<SapMessage>
{
    SapMessage message{};
    auto const offset = parse_header(packet, message);
    if (!offset || !parse_payload(statusbar::as_string_view(packet.subspan(*offset)), message)) {
        return std::nullopt;
    }
    return message;
}

auto origin_line(std::string_view sdp) noexcept -> std::string_view
{
    while (!sdp.empty()) {
        auto const end = sdp.find('\n');
        auto const line = sdp.substr(0, end == std::string_view::npos ? sdp.size() : end + 1);
        if (line.starts_with("o=")) {
            return line;
        }
        sdp.remove_prefix(line.size());
    }
    return {};
}

}  // namespace padl::sap
