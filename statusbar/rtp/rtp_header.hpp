#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RTP - RFC 3550 section 5.1 fixed header, as carried by AES67 streams.
///
/// Transport-agnostic: everything here works on the UDP payload, so the same
/// code serves a UDP socket (which hands over the payload) and a raw path such
/// as AF_XDP (which peels the Ethernet, IPv4 and UDP headers first).

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::rtp {

using ieee::doublet_t;
using ieee::octet_t;
using ieee::quadlet_t;

/// RTP fixed header - RFC 3550 Figure 1 (12 bytes, network order)
struct RtpHeader
{
    /// Length of the fixed header on the wire
    static constexpr size_t LENGTH = 12;

    /// The only RTP version
    static constexpr uint8_t VERSION = 2;

    /// Byte 0: version[7:6] | padding[5] | extension[4] | csrc_count[3:0]
    octet_t version_p_x_cc;

    /// Byte 1: marker[7] | payload_type[6:0]
    octet_t m_pt;

    /// Bytes 2-3: sequence number
    doublet_t sequence;

    /// Bytes 4-7: timestamp (media clock samples)
    quadlet_t timestamp;

    /// Bytes 8-11: synchronisation source
    quadlet_t ssrc;

    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return version_p_x_cc.get_bits(0xC0U, 6); }
    [[nodiscard]] constexpr auto padding() const noexcept -> bool { return version_p_x_cc.has_flag(0x20U); }
    [[nodiscard]] constexpr auto extension() const noexcept -> bool { return version_p_x_cc.has_flag(0x10U); }
    [[nodiscard]] constexpr auto csrc_count() const noexcept -> uint8_t { return version_p_x_cc.get_bits(0x0FU, 0); }
    [[nodiscard]] constexpr auto marker() const noexcept -> bool { return m_pt.has_flag(0x80U); }
    [[nodiscard]] constexpr auto payload_type() const noexcept -> uint8_t { return m_pt.get_bits(0x7FU, 0); }

    constexpr void set_marker(bool const value) noexcept { m_pt.set_flag(0x80U, value); }
    constexpr void set_payload_type(uint8_t const value) noexcept { m_pt.set_bits(0x7FU, 0, value); }

    /// A version 2 header with no padding, extension or CSRCs: what AES67 senders emit.
    constexpr void init(uint8_t const payload_type, uint16_t const seq, uint32_t const ts, uint32_t const source) noexcept
    {
        version_p_x_cc = static_cast<uint8_t>(VERSION << 6U);
        m_pt = 0U;
        set_payload_type(payload_type);
        sequence = seq;
        timestamp = ts;
        ssrc = source;
    }

    auto operator<=>(RtpHeader const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(RtpHeader) == RtpHeader::LENGTH, "RtpHeader must be exactly 12 bytes");
static_assert(alignof(RtpHeader) == 1, "RtpHeader must be byte aligned");
static_assert(offsetof(RtpHeader, sequence) == 2, "sequence must be at offset 2");
static_assert(offsetof(RtpHeader, timestamp) == 4, "timestamp must be at offset 4");
static_assert(offsetof(RtpHeader, ssrc) == 8, "ssrc must be at offset 8");

/// A received RTP packet: its fixed header and its payload, a view into the
/// packet with the CSRC list, header extension and padding removed. AES67
/// streams carry none of those, so they are skipped rather than exposed.
struct RtpPacket
{
    RtpHeader header;
    std::span<uint8_t const> payload;
};

/// Parse an RTP packet from a UDP payload. nullopt unless it is a well-formed
/// version 2 packet: the CSRC list and any header extension must fit, and a
/// padding count must be nonzero and fit in what follows them. The payload type
/// is not judged: AES67 never multiplexes RTCP on the RTP port, so a marked
/// packet of any payload type is RTP, and the caller matches the type to the
/// stream's SDP.
[[nodiscard]] auto parse_rtp(std::span<uint8_t const> datagram) noexcept -> std::optional<RtpPacket>;

/// Write `header` and `payload` into `datagram` as a fixed-header-only packet:
/// the padding, extension and CSRC count bits are cleared, since none of those
/// sections is written. Returns the datagram length, or nullopt if it does not fit.
[[nodiscard]] auto serialize_rtp(RtpHeader const& header, std::span<uint8_t const> payload, std::span<uint8_t> datagram) noexcept
    -> std::optional<size_t>;

}  // namespace statusbar::rtp

// Serialization traits - RtpHeader is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::rtp::RtpHeader> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::rtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::rtp
