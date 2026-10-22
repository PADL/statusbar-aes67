// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtp_header.hpp"

namespace statusbar::rtp {

auto parse_rtp(std::span<uint8_t const> const datagram) noexcept -> std::optional<RtpPacket>
{
    RtpPacket packet{};
    BufferDeserializerBuilder reader{datagram};
    reader.parse(&packet.header);
    if (!reader || packet.header.version() != RtpHeader::VERSION) {
        return std::nullopt;
    }

    reader.skip(size_t{packet.header.csrc_count()} * 4);
    if (packet.header.extension()) {
        doublet_t profile{};
        doublet_t length{};  // in 32-bit words, excluding this 4-byte header
        reader.parse(&profile).parse(&length).skip(size_t{length.get()} * 4);
    }
    if (!reader) {
        return std::nullopt;
    }

    size_t const start = reader.position();
    size_t end = datagram.size();
    if (packet.header.padding()) {
        // The last octet counts the padding octets, itself included.
        size_t const pad = datagram.back();
        if (pad == 0 || pad > end - start) {
            return std::nullopt;
        }
        end -= pad;
    }
    packet.payload = datagram.subspan(start, end - start);
    return packet;
}

auto serialize_rtp(RtpHeader const& header, std::span<uint8_t const> const payload, std::span<uint8_t> const datagram) noexcept
    -> std::optional<size_t>
{
    RtpHeader fixed = header;
    fixed.version_p_x_cc = static_cast<uint8_t>(RtpHeader::VERSION << 6U);
    BufferSerializerBuilderWithBuffer writer{datagram};
    writer.append(fixed).append(payload);
    if (!writer) {
        return std::nullopt;
    }
    return writer.position();
}

}  // namespace statusbar::rtp
