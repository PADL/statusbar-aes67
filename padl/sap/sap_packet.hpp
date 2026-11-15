#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RFC 2974 SAP packets: the Section 3 header, written into a caller's buffer and read through
/// buffer, and the SDP payload they carry.

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/status/status.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace padl::sap {

using statusbar::ieee::doublet_t;
using statusbar::ieee::octet_t;
using statusbar::ieee::quadlet_t;

/// The SAP payload type field: SDP.
inline constexpr std::string_view SAP_PAYLOAD_TYPE = "application/sdp";

/// The largest SAP packet sent or read: room for a large ST 2110-30 or ST 2022-7 description, sent fragmented
/// (RFC 2974 Section 6 only recommends under 1 KB).
inline constexpr size_t MAX_SAP_PACKET = 4096;

/// SAP header - RFC 2974 Section 3 (8 bytes, network order), with an IPv4 originating source
struct SapHeader
{
    /// Length of the header on the wire, before any authentication data
    static constexpr size_t LENGTH = 8;

    /// The only SAP version
    static constexpr uint8_t VERSION = 1;

    /// The flags serialize_sap_packet() writes as given: the version (V) and message type (T)
    static constexpr uint8_t SENT_FLAGS = 0xE4;

    /// Byte 0: version[7:5] | address_type[4] | reserved[3] | message_type[2] | encrypted[1] | compressed[0]
    octet_t flags;

    /// Byte 1: authentication data length, in 32-bit words
    octet_t auth_length;

    /// Bytes 2-3: message identifier hash
    doublet_t message_id_hash;

    /// Bytes 4-7: originating source
    quadlet_t origin;

    /// The SAP version (V): VERSION in every packet this reads.
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return flags.get_bits(0xE0U, 5); }
    /// Whether the originating source is IPv6 (A), which this does not read.
    [[nodiscard]] constexpr auto ipv6() const noexcept -> bool { return flags.has_flag(0x10U); }
    /// Whether the message is a deletion (T), not an announcement.
    [[nodiscard]] constexpr auto deletion() const noexcept -> bool { return flags.has_flag(0x04U); }
    /// Whether the payload is encrypted (E), which this does not read.
    [[nodiscard]] constexpr auto encrypted() const noexcept -> bool { return flags.has_flag(0x02U); }
    /// Whether the payload is compressed (C), which this does not read.
    [[nodiscard]] constexpr auto compressed() const noexcept -> bool { return flags.has_flag(0x01U); }

    /// A version 1 header with an IPv4 origin, no authentication, encryption or compression.
    constexpr void init(bool const is_deletion, uint16_t const hash, statusbar::ip::IPv4Address const& source) noexcept
    {
        flags = static_cast<uint8_t>(VERSION << 5U);
        flags.set_flag(0x04U, is_deletion);
        auth_length = 0U;
        message_id_hash = hash;
        origin = source.to_uint32();
    }

    auto operator<=>(SapHeader const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(SapHeader) == SapHeader::LENGTH, "SapHeader must be exactly 8 bytes");
static_assert(alignof(SapHeader) == 1, "SapHeader must be byte aligned");
static_assert(offsetof(SapHeader, flags) == 0, "flags must be at offset 0");
static_assert(offsetof(SapHeader, auth_length) == 1, "auth_length must be at offset 1");
static_assert(offsetof(SapHeader, message_id_hash) == 2, "message_id_hash must be at offset 2");
static_assert(offsetof(SapHeader, origin) == 4, "origin must be at offset 4");

/// The length of a SAP packet carrying `sdp` as serialize_sap_packet() writes it: header, payload type and text.
[[nodiscard]] constexpr auto sap_packet_length(std::string_view const sdp) noexcept -> size_t
{
    return SapHeader::LENGTH + SAP_PAYLOAD_TYPE.size() + 1 + sdp.size();
}

/// Write a SAP packet into `datagram`: `header` with only its version and message type kept (SENT_FLAGS: never an
/// IPv6 origin, encryption or compression) and no authentication data, the payload type and `sdp`. Returns the
/// packet length, or the buffer's error when it does not fit.
[[nodiscard]] auto serialize_sap_packet(SapHeader const& header, std::string_view sdp, std::span<uint8_t> datagram) noexcept
    -> statusbar::StatusValue<size_t>;

/// What a SAP packet says, for tests and listeners: nullopt unless it is a
/// well-formed version 1 IPv4 packet with no encryption or compression.
struct SapMessage
{
    bool deletion{false};
    uint16_t message_id_hash{0};
    statusbar::ip::IPv4Address origin;
    std::string payload_type;  ///< empty when absent
    std::string_view sdp;      ///< a view into the packet
};
[[nodiscard]] auto parse_sap_packet(std::span<uint8_t const> packet) -> std::optional<SapMessage>;

/// The o= line of an SDP text, with its line ending, as a deletion carries it (RFC 2974 Section 3); empty for none.
[[nodiscard]] auto origin_line(std::string_view sdp) noexcept -> std::string_view;

}  // namespace padl::sap

// Serialization traits - SapHeader is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<padl::sap::SapHeader> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace padl::sap {
using statusbar::protocol::load_unchecked;
using statusbar::protocol::store_unchecked;
}  // namespace padl::sap
