// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtp_header.hpp"

#include <algorithm>

namespace statusbar::rtp {

namespace {

/// Skips the CSRC list and any header extension (RFC 3550 Section 5.3.1); false when the datagram is too short.
[[nodiscard]] auto skip_csrcs_and_extension(RtpHeader const& header, BufferDeserializerBuilder& reader) noexcept -> bool
{
    reader.skip(size_t{header.csrc_count()} * 4);
    if (header.extension()) {
        doublet_t profile{};
        doublet_t length{};  // in 32-bit words, excluding this 4-byte header
        reader.parse(&profile).parse(&length).skip(size_t{length.get()} * 4);
    }
    return static_cast<bool>(reader);
}

/// The end of the payload before any padding (RFC 3550 Section 5.1); std::nullopt when the padding count is invalid.
[[nodiscard]] auto payload_end(RtpHeader const& header, std::span<uint8_t const> const datagram, size_t const start) noexcept
    -> std::optional<size_t>
{
    if (!header.padding()) {
        return datagram.size();
    }
    size_t const pad = datagram.back();  // counts the padding octets, itself included
    if (pad == 0 || pad > datagram.size() - start) {
        return std::nullopt;
    }
    return datagram.size() - pad;
}

/// Sets the packet's payload from what follows its fixed header in the reader; false when the datagram is malformed.
[[nodiscard]] auto parse_payload(
    BufferDeserializerBuilder& reader, std::span<uint8_t const> const datagram, RtpPacket& packet) noexcept -> bool
{
    if (!skip_csrcs_and_extension(packet.header, reader)) {
        return false;
    }
    size_t const start = reader.position();
    auto const end = payload_end(packet.header, datagram, start);
    if (!end) {
        return false;
    }
    packet.payload = datagram.subspan(start, *end - start);
    return true;
}

}  // namespace

auto parse_rtp(std::span<uint8_t const> const datagram) noexcept -> std::optional<RtpPacket>
{
    RtpPacket packet{};
    BufferDeserializerBuilder reader{datagram};
    reader.parse(&packet.header);
    if (!reader || packet.header.version() != RtpHeader::VERSION || !parse_payload(reader, datagram, packet)) {
        return std::nullopt;
    }
    return packet;
}

auto serialize_rtp_header(RtpHeader const& header, size_t const payload_length, std::span<uint8_t> const datagram) noexcept
    -> std::optional<size_t>
{
    if (datagram.size() < RtpHeader::LENGTH || datagram.size() - RtpHeader::LENGTH < payload_length) {
        return std::nullopt;
    }
    RtpHeader fixed = header;
    fixed.version_p_x_cc = static_cast<uint8_t>(RtpHeader::VERSION << 6U);
    BufferSerializerBuilderWithBuffer writer{datagram};
    if (!writer.append(fixed)) {
        return std::nullopt;
    }
    return writer.position();
}

auto serialize_rtp(RtpHeader const& header, std::span<uint8_t const> const payload, std::span<uint8_t> const datagram) noexcept
    -> std::optional<size_t>
{
    auto const offset = serialize_rtp_header(header, payload.size(), datagram);
    if (!offset) {
        return std::nullopt;
    }
    std::ranges::copy(payload, datagram.begin() + static_cast<std::ptrdiff_t>(*offset));
    return *offset + payload.size();
}

}  // namespace statusbar::rtp
