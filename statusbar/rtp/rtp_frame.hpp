#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RTP in a raw Ethernet frame: Ethernet II (optionally 802.1Q-tagged), IPv4 and UDP around an
/// RFC 3550 packet, for paths that see whole frames (AF_XDP) rather than a socket's UDP payload.

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
    /// The DSCP media is marked with by default: 46 (EF). AES67-2018 Section 6.2 recommends
    /// AF41 (34) for media and EF for PTP; a sender may mark with another (dscp below).
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

/// The Ethernet group address an IPv4 multicast address maps to (RFC 1112 §6.4):
/// 01:00:5e and the address's low 23 bits.
[[nodiscard]] constexpr auto multicast_mac(ip::IPv4Address const& group) noexcept -> ieee::Eui48
{
    return ieee::Eui48{0x01, 0x00, 0x5E, static_cast<uint8_t>(group.octet(1) & 0x7FU), group.octet(2), group.octet(3)};
}

/// Parse an Ethernet frame carrying RTP over UDP over IPv4: nullopt unless it is unfragmented
/// IPv4 (IHL >= 5, a valid checksum) and UDP, with lengths agreeing with each other and the
/// frame, and parse_rtp() accepts the payload. Addresses, ports and payload type are not judged.
[[nodiscard]] auto parse_rtp_frame(std::span<uint8_t const> frame) noexcept -> std::optional<RtpFrame>;

/// Write an Ethernet frame carrying `header` and `payload` to `addressing`: the frame length, or
/// nullopt if it does not fit `buffer` or exceeds IPv4's 65535 bytes. The NIC pads short frames.
[[nodiscard]] auto serialize_rtp_frame(
    FrameAddressing const& addressing,
    RtpHeader const& header,
    std::span<uint8_t const> payload,
    std::span<uint8_t> buffer) noexcept -> std::optional<size_t>;

/// Write serialize_rtp_frame()'s headers for a `payload_length`-byte payload, leaving the payload
/// to the caller: its offset in `buffer`, or nullopt where serialize_rtp_frame() would fail.
[[nodiscard]] auto serialize_rtp_frame_headers(
    FrameAddressing const& addressing, RtpHeader const& header, size_t payload_length, std::span<uint8_t> buffer) noexcept
    -> std::optional<size_t>;

}  // namespace statusbar::rtp
