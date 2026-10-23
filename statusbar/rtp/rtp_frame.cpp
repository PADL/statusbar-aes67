// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtp_frame.hpp"

#include "statusbar/ip/ip_ipv4.hpp"
#include "statusbar/ip/ip_udp.hpp"

#include <algorithm>
#include <limits>

namespace statusbar::rtp {

namespace {

constexpr uint16_t ETHERTYPE_IPV4 = 0x0800;
constexpr uint16_t IPV4_MORE_FRAGMENTS = 0x2000;
constexpr uint16_t IPV4_FRAGMENT_OFFSET = 0x1FFF;

/// The ones' complement sum of a header, options included; 0 when its
/// checksum field is right (RFC 791, RFC 1071).
auto header_checksum(std::span<uint8_t const> const header) noexcept -> uint16_t
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < header.size(); i += 2) {
        sum += static_cast<uint32_t>((header[i] << 8U) | header[i + 1]);
    }
    while ((sum >> 16U) != 0) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return static_cast<uint16_t>(~sum);
}

}  // namespace

auto parse_rtp_frame(std::span<uint8_t const> const frame) noexcept -> std::optional<RtpFrame>
{
    ieee::EthernetFrame ethernet{};
    BufferDeserializerBuilder reader{frame};
    reader.parse(&ethernet);
    if (!reader || ethernet.ethertype.get() != ETHERTYPE_IPV4) {
        return std::nullopt;
    }

    size_t const ip_start = reader.position();
    ip::IPv4Header ipv4{};
    reader.parse(&ipv4);
    if (!reader || ipv4.version() != 4 || ipv4.ihl() < 5 || ipv4.protocol.get() != ip::IPv4Header::PROTOCOL_UDP) {
        return std::nullopt;
    }
    size_t const ip_header_length = ipv4.header_length();
    size_t const ip_total_length = ipv4.total_length.get();
    if (ip_total_length < ip_header_length + ip::UdpHeader::LENGTH || ip_total_length > frame.size() - ip_start) {
        return std::nullopt;
    }
    if ((ipv4.flags_fragment.get() & (IPV4_MORE_FRAGMENTS | IPV4_FRAGMENT_OFFSET)) != 0) {
        return std::nullopt;
    }
    if (header_checksum(frame.subspan(ip_start, ip_header_length)) != 0) {
        return std::nullopt;
    }

    ip::UdpHeader udp{};
    reader.skip(ip_header_length - ip::IPv4Header::MIN_LENGTH).parse(&udp);
    if (!reader || udp.length.get() != ip_total_length - ip_header_length) {
        return std::nullopt;
    }

    auto const datagram = frame.subspan(reader.position(), udp.payload_length());
    auto const packet = parse_rtp(datagram);
    if (!packet) {
        return std::nullopt;
    }

    RtpFrame result{};
    result.addressing.destination_mac = ethernet.dest_mac;
    result.addressing.source_mac = ethernet.src_mac;
    if (ethernet.vlan_tag.is_set()) {
        result.addressing.vlan = ethernet.vlan_tag;
    }
    result.addressing.source_address = ipv4.src_addr;
    result.addressing.destination_address = ipv4.dst_addr;
    result.addressing.source_port = udp.src_port.get();
    result.addressing.destination_port = udp.dst_port.get();
    result.addressing.ttl = ipv4.ttl.get();
    result.addressing.dscp = ipv4.dscp();
    result.datagram = datagram;
    result.packet = *packet;
    return result;
}

auto serialize_rtp_frame_headers(
    FrameAddressing const& addressing,
    RtpHeader const& header,
    size_t const payload_length,
    std::span<uint8_t> const buffer) noexcept -> std::optional<size_t>
{
    if (payload_length >
        std::numeric_limits<uint16_t>::max() - ip::IPv4Header::MIN_LENGTH - ip::UdpHeader::LENGTH - RtpHeader::LENGTH) {
        return std::nullopt;
    }
    size_t const rtp_length = RtpHeader::LENGTH + payload_length;
    auto const udp_length = static_cast<uint16_t>(ip::UdpHeader::LENGTH + rtp_length);

    ieee::EthernetFrame ethernet{};
    ethernet.dest_mac = addressing.destination_mac;
    ethernet.src_mac = addressing.source_mac;
    ethernet.vlan_tag = addressing.vlan.value_or(ieee::make_empty_vlan_tag());
    ethernet.ethertype = ETHERTYPE_IPV4;

    ip::IPv4Header ipv4{};
    ipv4.init(ip::IPv4Header::PROTOCOL_UDP, udp_length, addressing.ttl);  // DF set, id 0
    ipv4.set_dscp(addressing.dscp);
    ipv4.src_addr = addressing.source_address;
    ipv4.dst_addr = addressing.destination_address;
    ip::update_ipv4_checksum(ipv4);

    ip::UdpHeader udp{};
    udp.init(addressing.source_port, addressing.destination_port, static_cast<uint16_t>(rtp_length));

    BufferSerializerBuilderWithBuffer writer{buffer};
    writer.append(ethernet).append(ipv4).append(udp);
    if (!writer) {
        return std::nullopt;
    }
    size_t const headers = writer.position();
    auto const offset = serialize_rtp_header(header, payload_length, buffer.subspan(headers));
    if (!offset) {
        return std::nullopt;
    }
    return headers + *offset;
}

auto serialize_rtp_frame(
    FrameAddressing const& addressing,
    RtpHeader const& header,
    std::span<uint8_t const> const payload,
    std::span<uint8_t> const buffer) noexcept -> std::optional<size_t>
{
    auto const offset = serialize_rtp_frame_headers(addressing, header, payload.size(), buffer);
    if (!offset) {
        return std::nullopt;
    }
    std::ranges::copy(payload, buffer.begin() + static_cast<std::ptrdiff_t>(*offset));
    return *offset + payload.size();
}

}  // namespace statusbar::rtp
