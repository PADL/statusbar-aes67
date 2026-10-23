#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RTP in a raw Ethernet frame: Ethernet II (with an optional 802.1Q tag), IPv4
/// and UDP around an RFC 3550 packet, for paths that see whole frames (AF_XDP)
/// rather than a socket's UDP payload.
///
/// parse_rtp_frame() refuses anything that is not unfragmented IPv4/UDP with a
/// correct header checksum and lengths that agree with each other and with the
/// frame (trailing Ethernet padding is allowed); it does not judge addresses,
/// ports or the payload type, which are the caller's to match.
/// serialize_rtp_frame() writes what an AES67 sender emits: IHL 5, DF set, the
/// header checksum computed, and a zero (absent) UDP checksum, which IPv4
/// permits and AES67 receivers accept.

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/rtp/rtp_header.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::rtp {

/// The headers around an RTP packet in a frame: what serialize_rtp_frame()
/// writes and parse_rtp_frame() reports.
struct FrameAddressing
{
    /// Expedited Forwarding: the DSCP AES67 recommends for media (AES67 section 6.1)
    static constexpr uint8_t DSCP_MEDIA = 46;

    ieee::Eui48 destination_mac{};
    ieee::Eui48 source_mac{};
    std::optional<ieee::VlanTag> vlan;  ///< written when set; reported when the frame is tagged
    ip::IPv4Address source_address;
    ip::IPv4Address destination_address;
    uint16_t source_port{0};
    uint16_t destination_port{0};
    uint8_t ttl{1};
    uint8_t dscp{DSCP_MEDIA};
};

/// A received frame's addressing, its UDP payload and the RTP packet parsed
/// from it; both spans are views into the frame.
struct RtpFrame
{
    FrameAddressing addressing;
    std::span<uint8_t const> datagram;  ///< the UDP payload: what a socket would have delivered
    RtpPacket packet;
};

/// Bytes the headers take before the RTP packet: 42 untagged, 46 tagged.
[[nodiscard]] constexpr auto rtp_frame_overhead(bool const tagged) noexcept -> size_t
{
    return 14 + (tagged ? ieee::VlanTag::LENGTH : 0) + 20 + 8;
}

/// The Ethernet group address an IPv4 multicast address maps to (RFC 1112
/// section 6.4): 01:00:5e and the address's low 23 bits.
[[nodiscard]] constexpr auto multicast_mac(ip::IPv4Address const& group) noexcept -> ieee::Eui48
{
    return ieee::Eui48{0x01, 0x00, 0x5E, static_cast<uint8_t>(group.octet(1) & 0x7FU), group.octet(2), group.octet(3)};
}

/// Parse an Ethernet frame carrying RTP over UDP over IPv4. nullopt unless the
/// EtherType (after any 802.1Q tag) is IPv4, the IPv4 header is version 4 with
/// IHL >= 5 and a valid checksum over its whole length, it is not a fragment,
/// the protocol is UDP, the IPv4 total length fits in the frame, the UDP length
/// equals the IPv4 payload length, and parse_rtp() accepts the UDP payload.
[[nodiscard]] auto parse_rtp_frame(std::span<uint8_t const> frame) noexcept -> std::optional<RtpFrame>;

/// Write an Ethernet frame carrying `header` and `payload` (as serialize_rtp()
/// writes them) to `addressing`. Returns the frame length, or nullopt if it
/// does not fit in `buffer` or the IPv4 total length would exceed 65535.
/// Short frames are not padded to the Ethernet minimum; the NIC pads.
[[nodiscard]] auto serialize_rtp_frame(
    FrameAddressing const& addressing,
    RtpHeader const& header,
    std::span<uint8_t const> payload,
    std::span<uint8_t> buffer) noexcept -> std::optional<size_t>;

/// Write the headers serialize_rtp_frame() writes for a payload of
/// `payload_length` bytes, the RTP header last, leaving the payload for the
/// caller to write in place after them. Returns the payload's offset in
/// `buffer`, or nullopt under the conditions serialize_rtp_frame() fails.
[[nodiscard]] auto serialize_rtp_frame_headers(
    FrameAddressing const& addressing, RtpHeader const& header, size_t payload_length, std::span<uint8_t> buffer) noexcept
    -> std::optional<size_t>;

}  // namespace statusbar::rtp
