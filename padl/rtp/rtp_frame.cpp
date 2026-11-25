// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/rtp/rtp_frame.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/checksum/checksum_ones_complement.hpp"
#include "statusbar/ip/ip_ipv4.hpp"
#include "statusbar/ip/ip_udp.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace padl::rtp {

namespace {

constexpr uint16_t ETHERTYPE_IPV4 = 0x0800;
constexpr uint16_t IPV4_MORE_FRAGMENTS = 0x2000;
constexpr uint16_t IPV4_FRAGMENT_OFFSET = 0x1FFF;

/// True when the header is an unfragmented IPv4 header carrying UDP.
[[nodiscard]] auto is_unfragmented_udp(statusbar::ip::IPv4Header const& ipv4) noexcept -> bool
{
    return ipv4.version() == 4 && ipv4.ihl() >= 5 && ipv4.protocol.get() == statusbar::ip::IPv4Header::PROTOCOL_UDP &&
        (ipv4.flags_fragment.get() & (IPV4_MORE_FRAGMENTS | IPV4_FRAGMENT_OFFSET)) == 0;
}

/// True when the IPv4 packet starting at `packet` fits it, can hold a UDP header, and its header checksum is right.
[[nodiscard]] auto fits_with_valid_checksum(statusbar::ip::IPv4Header const& ipv4, std::span<uint8_t const> const packet) noexcept
    -> bool
{
    size_t const header_length = ipv4.header_length();
    size_t const total_length = ipv4.total_length.get();
    return total_length >= header_length + statusbar::ip::UdpHeader::LENGTH && total_length <= packet.size() &&
        statusbar::checksum::internet_checksum_ok(packet.first(header_length));
}

/// Parses and validates the IPv4 header at the reader's position; std::nullopt when it is not a whole UDP datagram.
[[nodiscard]] auto parse_ipv4(statusbar::BufferDeserializerBuilder& reader, std::span<uint8_t const> const frame) noexcept
    -> std::optional<statusbar::ip::IPv4Header>
{
    size_t const ip_start = reader.position();
    statusbar::ip::IPv4Header ipv4{};
    reader.parse(&ipv4);
    if (!reader || !is_unfragmented_udp(ipv4) || !fits_with_valid_checksum(ipv4, frame.subspan(ip_start))) {
        return std::nullopt;
    }
    return ipv4;
}

/// The addressing a received frame's Ethernet, IPv4 and UDP headers carry.
[[nodiscard]] auto frame_addressing(
    statusbar::ieee::EthernetFrame const& ethernet,
    statusbar::ip::IPv4Header const& ipv4,
    statusbar::ip::UdpHeader const& udp) noexcept -> FrameAddressing
{
    return FrameAddressing{
        .destination_mac = ethernet.dest_mac,
        .source_mac = ethernet.src_mac,
        .vlan = ethernet.vlan_tag.is_set() ? std::optional{ethernet.vlan_tag} : std::nullopt,
        .source_address = ipv4.src_addr,
        .destination_address = ipv4.dst_addr,
        .source_port = udp.src_port.get(),
        .destination_port = udp.dst_port.get(),
        .ttl = ipv4.ttl.get(),
        .dscp = ipv4.dscp(),
    };
}

/// Parses the IPv4 and UDP headers following `ethernet`; std::nullopt when either is malformed.
[[nodiscard]] auto parse_ipv4_udp(
    statusbar::BufferDeserializerBuilder& reader,
    std::span<uint8_t const> const frame,
    statusbar::ieee::EthernetFrame const& ethernet) noexcept -> std::optional<UdpFrame>
{
    auto const ipv4 = parse_ipv4(reader, frame);
    if (!ipv4) {
        return std::nullopt;
    }
    statusbar::ip::UdpHeader udp{};
    reader.skip(ipv4->header_length() - statusbar::ip::IPv4Header::MIN_LENGTH).parse(&udp);
    if (!reader || udp.length.get() != ipv4->total_length.get() - ipv4->header_length()) {
        return std::nullopt;
    }
    return UdpFrame{
        .addressing = frame_addressing(ethernet, *ipv4, udp), .datagram = frame.subspan(reader.position(), udp.payload_length())};
}

/// The Ethernet header for an IPv4 frame to `addressing`, tagged when it carries a VLAN.
[[nodiscard]] auto ethernet_header(FrameAddressing const& addressing) noexcept -> statusbar::ieee::EthernetFrame
{
    statusbar::ieee::EthernetFrame ethernet{};
    ethernet.dest_mac = addressing.destination_mac;
    ethernet.src_mac = addressing.source_mac;
    ethernet.vlan_tag = addressing.vlan.value_or(statusbar::ieee::make_empty_vlan_tag());
    ethernet.ethertype = ETHERTYPE_IPV4;
    return ethernet;
}

/// The IPv4 header, DF set and id 0, for a UDP datagram carrying `udp_payload_length` octets to `addressing`.
[[nodiscard]] auto ipv4_header(FrameAddressing const& addressing, size_t const udp_payload_length) noexcept
    -> statusbar::ip::IPv4Header
{  // NOLINT(statusbar-block-statements): the IPv4 header, field by field
    statusbar::ip::IPv4Header ipv4{};
    ipv4.init(
        statusbar::ip::IPv4Header::PROTOCOL_UDP,
        static_cast<uint16_t>(statusbar::ip::UdpHeader::LENGTH + udp_payload_length),
        addressing.ttl);
    ipv4.set_dscp(addressing.dscp);
    ipv4.src_addr = addressing.source_address;
    ipv4.dst_addr = addressing.destination_address;
    statusbar::ip::update_ipv4_checksum(ipv4);
    return ipv4;
}

/// The UDP header for `payload_length` octets between `addressing`'s ports.
[[nodiscard]] auto udp_header(FrameAddressing const& addressing, uint16_t const payload_length) noexcept -> statusbar::ip::UdpHeader
{
    statusbar::ip::UdpHeader udp{};
    udp.init(addressing.source_port, addressing.destination_port, payload_length);
    return udp;
}

/// True when an RTP payload of `payload_length` octets fits one IPv4 datagram with its headers.
[[nodiscard]] constexpr auto fits_ipv4(size_t const payload_length) noexcept -> bool
{
    return payload_length <= std::numeric_limits<uint16_t>::max() - statusbar::ip::IPv4Header::MIN_LENGTH -
        statusbar::ip::UdpHeader::LENGTH - RtpHeader::LENGTH;
}

/// Appends the Ethernet, IPv4 and UDP headers around an `rtp_length`-octet RTP packet to `addressing`; false when
/// they do not fit the writer.
[[nodiscard]] auto append_udp_headers(
    statusbar::BufferSerializerBuilderWithBuffer& writer, FrameAddressing const& addressing, size_t const rtp_length) noexcept
    -> bool
{
    return static_cast<bool>(writer.append(ethernet_header(addressing))
                                 .append(ipv4_header(addressing, rtp_length))
                                 .append(udp_header(addressing, static_cast<uint16_t>(rtp_length))));
}

/// A template whose RTP header starts at `rtp`: its length and the offsets of its varying fields.
[[nodiscard]] auto template_at(FrameHeaderTemplate result, size_t const rtp) noexcept -> FrameHeaderTemplate
{
    result.length = rtp + RtpHeader::LENGTH;
    result.sequence_offset = rtp + offsetof(RtpHeader, sequence);
    result.timestamp_offset = rtp + offsetof(RtpHeader, timestamp);
    result.ssrc_offset = rtp + offsetof(RtpHeader, ssrc);
    return result;
}

/// `result` with the RTP header of `payload_type`, sequence, timestamp and SSRC zero, at `rtp`; nullopt when it does not
/// fit.
[[nodiscard]] auto with_rtp_header(FrameHeaderTemplate result, size_t const rtp, uint8_t const payload_type) noexcept
    -> std::optional<FrameHeaderTemplate>
{
    RtpHeader header{};
    header.init(payload_type, 0, 0, 0);
    if (!serialize_rtp_header(header, 0, std::span{result.bytes}.subspan(rtp))) {
        return std::nullopt;
    }
    return template_at(result, rtp);
}

}  // namespace

auto parse_udp_frame(std::span<uint8_t const> const frame) noexcept -> std::optional<UdpFrame>
{
    statusbar::ieee::EthernetFrame ethernet{};
    statusbar::BufferDeserializerBuilder reader{frame};
    reader.parse(&ethernet);
    if (!reader || ethernet.ethertype.get() != ETHERTYPE_IPV4) {
        return std::nullopt;
    }
    return parse_ipv4_udp(reader, frame, ethernet);
}

auto parse_rtp_frame(std::span<uint8_t const> const frame) noexcept -> std::optional<RtpFrame>
{
    auto const udp = parse_udp_frame(frame);
    if (!udp) {
        return std::nullopt;
    }
    auto const packet = parse_rtp(udp->datagram);
    if (!packet) {
        return std::nullopt;
    }
    return RtpFrame{.addressing = udp->addressing, .datagram = udp->datagram, .packet = *packet};
}

auto serialize_rtp_frame_headers(
    FrameAddressing const& addressing,
    RtpHeader const& header,
    size_t const payload_length,
    std::span<uint8_t> const buffer) noexcept -> std::optional<size_t>
{
    if (!fits_ipv4(payload_length)) {
        return std::nullopt;
    }
    statusbar::BufferSerializerBuilderWithBuffer writer{buffer};
    if (!append_udp_headers(writer, addressing, RtpHeader::LENGTH + payload_length)) {
        return std::nullopt;
    }
    size_t const headers = writer.position();
    return serialize_rtp_header(header, payload_length, buffer.subspan(headers)).transform([headers](size_t const offset) {
        return headers + offset;
    });
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

auto frame_header_template(FrameAddressing const& addressing, uint8_t const payload_type, size_t const payload_length) noexcept
    -> std::optional<FrameHeaderTemplate>
{
    FrameHeaderTemplate result{};
    statusbar::BufferSerializerBuilderWithBuffer writer{std::span{result.bytes}};
    if (!fits_ipv4(payload_length) || !append_udp_headers(writer, addressing, RtpHeader::LENGTH + payload_length)) {
        return std::nullopt;
    }
    return with_rtp_header(result, writer.position(), payload_type);
}

auto FrameHeaderTemplate::write(
    uint16_t const sequence, uint32_t const timestamp, uint32_t const ssrc, std::span<uint8_t> const out) const noexcept -> bool
{
    if (out.size() < length) {
        return false;
    }
    statusbar::span_copy(out, header());
    statusbar::span_store(out.subspan(sequence_offset), doublet_t{sequence});
    statusbar::span_store(out.subspan(timestamp_offset), quadlet_t{timestamp});
    statusbar::span_store(out.subspan(ssrc_offset), quadlet_t{ssrc});
    return true;
}

}  // namespace padl::rtp
