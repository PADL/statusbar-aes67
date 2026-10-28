// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtp_frame.hpp"

#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using namespace statusbar;
using namespace statusbar::rtp;

namespace {

// 192.168.0.1:5004 -> 239.69.1.1:5004, DSCP 46, TTL 1, DF; the RTP packet of
// rtp_test's BASIC (PT 96, seq 0x1234, ts 0x89ABCDEF, SSRC 0x01020304, 4 bytes).
std::vector<uint8_t> const FRAME{0x01, 0x00, 0x5E, 0x45, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00,  // Ethernet
                                 0x45, 0xB8, 0x00, 0x2C, 0x00, 0x00, 0x40, 0x00, 0x01, 0x11, 0xC8, 0x19,              // IPv4
                                 0xC0, 0xA8, 0x00, 0x01, 0xEF, 0x45, 0x01, 0x01,                                      //
                                 0x13, 0x8C, 0x13, 0x8C, 0x00, 0x18, 0x00, 0x00,                                      // UDP
                                 0x80, 0x60, 0x12, 0x34, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x03, 0x04,              // RTP
                                 0xAA, 0xBB, 0xCC, 0xDD};

constexpr size_t IP = 14;  // offset of the IPv4 header in FRAME

auto bytes(std::span<uint8_t const> s) -> std::vector<uint8_t>
{
    return {s.begin(), s.end()};
}

// Rewrite the IPv4 header checksum of `frame` (header at `ip`) after an edit.
void fix_checksum(std::vector<uint8_t>& frame, size_t const ip)
{
    size_t const length = size_t{frame[ip] & 0x0FU} * 4;
    frame[ip + 10] = 0;
    frame[ip + 11] = 0;
    uint32_t sum = 0;
    for (size_t i = 0; i < length; i += 2) {
        sum += static_cast<uint32_t>((frame[ip + i] << 8U) | frame[ip + i + 1]);
    }
    while ((sum >> 16U) != 0) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    sum = ~sum & 0xFFFFU;
    frame[ip + 10] = static_cast<uint8_t>(sum >> 8U);
    frame[ip + 11] = static_cast<uint8_t>(sum);
}

/// An 8-channel L24 packet of 48 frames as one sender sends it: 10.10.32.163:5004 -> 239.200.163.140:5004,
/// DSCP 34, TTL 128, DF, UDP checksum 0, PT 99; `payload` octets of silence (1152 for 48 frames).
auto eight_channel_frame(size_t const payload) -> std::vector<uint8_t>
{
    std::vector<uint8_t> frame{0x01, 0x00, 0x5E, 0x48, 0xA3, 0x8C, 0x00, 0x50, 0xC2, 0xAC, 0x0A, 0x64, 0x08, 0x00,  // Ethernet
                               0x45, 0x88, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x80, 0x11, 0x00, 0x00,              // IPv4
                               0x0A, 0x0A, 0x20, 0xA3, 0xEF, 0xC8, 0xA3, 0x8C,                                      //
                               0x13, 0x8C, 0x13, 0x8C, 0x00, 0x00, 0x00, 0x00,                                      // UDP
                               0x80, 0x63, 0xCA, 0x34, 0x07, 0xAE, 0x9C, 0x40, 0x02, 0x41, 0xFA, 0x93};             // RTP
    frame.resize(frame.size() + payload, 0);
    size_t const udp_length = frame.size() - IP - 20;
    frame[IP + 2] = static_cast<uint8_t>((udp_length + 20) >> 8U);
    frame[IP + 3] = static_cast<uint8_t>(udp_length + 20);
    frame[IP + 24] = static_cast<uint8_t>(udp_length >> 8U);
    frame[IP + 25] = static_cast<uint8_t>(udp_length);
    fix_checksum(frame, IP);
    return frame;
}

}  // namespace

TEST(rtp_frame, parses_a_hand_assembled_frame)
{
    auto const parsed = parse_rtp_frame(FRAME);
    EXPECT_TRUE(parsed.has_value());
    auto const& a = parsed->addressing;
    EXPECT_TRUE(a.destination_mac == (ieee::Eui48{0x01, 0x00, 0x5E, 0x45, 0x01, 0x01}));
    EXPECT_TRUE(a.source_mac == (ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}));
    EXPECT_FALSE(a.vlan.has_value());
    EXPECT_TRUE(a.source_address == (ip::IPv4Address{192, 168, 0, 1}));
    EXPECT_TRUE(a.destination_address == (ip::IPv4Address{239, 69, 1, 1}));
    EXPECT_EQ(a.source_port, uint16_t{5004});
    EXPECT_EQ(a.destination_port, uint16_t{5004});
    EXPECT_EQ(a.ttl, uint8_t{1});
    EXPECT_EQ(a.dscp, uint8_t{46});
    EXPECT_EQ(parsed->packet.header.payload_type(), uint8_t{96});
    EXPECT_EQ(parsed->packet.header.sequence.get(), uint16_t{0x1234});
    EXPECT_EQ(parsed->packet.header.timestamp.get(), uint32_t{0x89ABCDEF});
    EXPECT_EQ(bytes(parsed->packet.payload), (std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD}));
    EXPECT_EQ(bytes(parsed->datagram), bytes(std::span{FRAME}.subspan(42)));
}

TEST(rtp_frame, serializes_the_same_bytes)
{
    auto const parsed = parse_rtp_frame(FRAME);
    EXPECT_TRUE(parsed.has_value());
    std::array<uint8_t, 128> buffer{};
    auto const length = serialize_rtp_frame(parsed->addressing, parsed->packet.header, parsed->packet.payload, buffer);
    EXPECT_TRUE(length.has_value());
    EXPECT_EQ(*length, FRAME.size());
    EXPECT_EQ(bytes(std::span{buffer}.first(*length)), FRAME);
    EXPECT_EQ(*length, rtp_frame_overhead(false) + RtpHeader::LENGTH + 4);
}

TEST(rtp_frame, writes_the_headers_for_a_payload_written_in_place)
{
    auto const parsed = parse_rtp_frame(FRAME);
    EXPECT_TRUE(parsed.has_value());
    std::array<uint8_t, 128> buffer{};
    auto const offset = serialize_rtp_frame_headers(parsed->addressing, parsed->packet.header, 4, buffer);
    EXPECT_TRUE(offset.has_value());
    EXPECT_EQ(*offset, rtp_frame_overhead(false) + RtpHeader::LENGTH);
    std::ranges::copy(parsed->packet.payload, buffer.begin() + static_cast<std::ptrdiff_t>(*offset));
    EXPECT_EQ(bytes(std::span{buffer}.first(*offset + 4)), FRAME);
    // The whole frame, payload included, must fit.
    std::vector<uint8_t> small(FRAME.size() - 1);
    EXPECT_FALSE(serialize_rtp_frame_headers(parsed->addressing, parsed->packet.header, 4, small).has_value());
    std::vector<uint8_t> big(70000);
    EXPECT_FALSE(serialize_rtp_frame_headers(parsed->addressing, parsed->packet.header, 65536 - 40, big).has_value());
    // The bare RTP header likewise.
    std::array<uint8_t, 16> datagram{};
    EXPECT_EQ(serialize_rtp_header(parsed->packet.header, 4, datagram), std::optional<size_t>{RtpHeader::LENGTH});
    EXPECT_TRUE(std::ranges::equal(std::span{datagram}.first(RtpHeader::LENGTH), std::span{FRAME}.subspan(42, RtpHeader::LENGTH)));
    EXPECT_FALSE(serialize_rtp_header(parsed->packet.header, 5, datagram).has_value());
}

TEST(rtp_frame, a_udp_frame_carrying_rtcp_parses_as_udp_only)
{
    // FRAME's headers around an 8-byte RR to port 5005: too short to be RTP.
    std::vector<uint8_t> frame{FRAME.begin(), FRAME.begin() + 42};
    frame.insert(frame.end(), {0x80, 0xC9, 0x00, 0x01, 0x01, 0x02, 0x03, 0x04});
    frame[IP + 3] = 36;         // IPv4 total length
    frame[IP + 20 + 3] = 0x8D;  // destination port 5005
    frame[IP + 20 + 5] = 16;    // UDP length
    fix_checksum(frame, IP);
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());
    auto const udp = parse_udp_frame(frame);
    EXPECT_TRUE(udp.has_value());
    EXPECT_EQ(udp->addressing.destination_port, uint16_t{5005});
    EXPECT_EQ(bytes(udp->datagram), bytes(std::span{frame}.subspan(42)));
}

TEST(rtp_frame, maps_a_group_to_its_mac)
{
    EXPECT_TRUE(multicast_mac(ip::IPv4Address{239, 69, 1, 1}) == (ieee::Eui48{0x01, 0x00, 0x5E, 0x45, 0x01, 0x01}));
    EXPECT_TRUE(multicast_mac(ip::IPv4Address{239, 255, 255, 255}) == (ieee::Eui48{0x01, 0x00, 0x5E, 0x7F, 0xFF, 0xFF}));
}

TEST(rtp_frame, round_trips_a_vlan_tagged_frame)
{
    auto parsed = parse_rtp_frame(FRAME);
    EXPECT_TRUE(parsed.has_value());
    parsed->addressing.vlan = ieee::VlanTag{100, false, 5};
    std::array<uint8_t, 128> buffer{};
    auto const length = serialize_rtp_frame(parsed->addressing, parsed->packet.header, parsed->packet.payload, buffer);
    EXPECT_TRUE(length.has_value());
    EXPECT_EQ(*length, FRAME.size() + 4);
    EXPECT_EQ(buffer[12], uint8_t{0x81});
    EXPECT_EQ(buffer[13], uint8_t{0x00});
    EXPECT_EQ(buffer[14], uint8_t{0xA0});  // PCP 5, DEI 0, VID 100
    EXPECT_EQ(buffer[15], uint8_t{0x64});

    auto const tagged = parse_rtp_frame(std::span{buffer}.first(*length));
    EXPECT_TRUE(tagged.has_value());
    EXPECT_TRUE(tagged->addressing.vlan.has_value());
    EXPECT_EQ(tagged->addressing.vlan->get_vid(), uint16_t{100});
    EXPECT_EQ(tagged->addressing.vlan->get_pcp(), uint8_t{5});
    EXPECT_EQ(bytes(tagged->packet.payload), (std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD}));
}

TEST(rtp_frame, accepts_ip_options_and_ethernet_padding)
{
    std::vector<uint8_t> frame = FRAME;
    // IHL 6: one word of options (four NOPs) after the fixed header.
    frame.insert(frame.begin() + IP + 20, {0x01, 0x01, 0x01, 0x01});
    frame[IP] = 0x46;
    frame[IP + 3] = 0x30;  // total length 48
    fix_checksum(frame, IP);
    frame.resize(frame.size() + 6, 0);  // padding past the IPv4 total length
    auto const parsed = parse_rtp_frame(frame);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->addressing.destination_port, uint16_t{5004});
    EXPECT_EQ(bytes(parsed->packet.payload), (std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD}));
}

TEST(rtp_frame, refuses_other_ethertypes_and_ipv6)
{
    std::vector<uint8_t> frame = FRAME;
    frame[12] = 0x22;  // AVTP
    frame[13] = 0xF0;
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());

    frame = FRAME;
    frame[12] = 0x86;  // IPv6 EtherType
    frame[13] = 0xDD;
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());

    frame = FRAME;
    frame[IP] = 0x65;  // version 6 behind an IPv4 EtherType
    fix_checksum(frame, IP);
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());
}

TEST(rtp_frame, refuses_non_udp)
{
    std::vector<uint8_t> frame = FRAME;
    frame[IP + 9] = 6;  // TCP
    fix_checksum(frame, IP);
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());
}

TEST(rtp_frame, refuses_a_bad_checksum)
{
    std::vector<uint8_t> frame = FRAME;
    frame[IP + 11] ^= 0x01U;
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());
}

TEST(rtp_frame, refuses_fragments)
{
    std::vector<uint8_t> frame = FRAME;
    frame[IP + 6] = 0x20;  // MF
    fix_checksum(frame, IP);
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());
}

TEST(rtp_frame, refuses_lengths_that_disagree)
{
    for (size_t n = 0; n < FRAME.size(); ++n) {
        EXPECT_FALSE(parse_rtp_frame(std::span{FRAME}.first(n)).has_value());  // truncated anywhere
    }

    std::vector<uint8_t> frame = FRAME;
    frame[IP + 3] = 0x2D;  // IPv4 total length past the frame
    fix_checksum(frame, IP);
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());

    frame = FRAME;
    frame[IP + 3] = 0x10;  // IPv4 total length shorter than its header and UDP
    fix_checksum(frame, IP);
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());

    frame = FRAME;
    frame[IP + 20 + 5] = 0x17;  // UDP length one short of the IPv4 payload
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());

    frame = FRAME;
    frame[IP] = 0x44;  // IHL 4
    fix_checksum(frame, IP);
    EXPECT_FALSE(parse_rtp_frame(frame).has_value());
}

TEST(rtp_frame, refuses_a_buffer_too_small)
{
    auto const parsed = parse_rtp_frame(FRAME);
    EXPECT_TRUE(parsed.has_value());
    std::vector<uint8_t> buffer(FRAME.size() - 1);
    EXPECT_FALSE(serialize_rtp_frame(parsed->addressing, parsed->packet.header, parsed->packet.payload, buffer).has_value());
    std::vector<uint8_t> const huge(65536 - 40);
    std::vector<uint8_t> big(70000);
    EXPECT_FALSE(serialize_rtp_frame(parsed->addressing, parsed->packet.header, huge, big).has_value());
}

TEST(rtp_frame, parses_an_eight_channel_packet_of_48_frames)
{
    auto const frame = eight_channel_frame(1152);
    EXPECT_EQ(frame.size(), size_t{1206});
    auto const parsed = parse_rtp_frame(frame);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed && parsed->addressing.destination_address == (ip::IPv4Address{239, 200, 163, 140}));
    EXPECT_TRUE(parsed && parsed->addressing.source_port == 5004 && parsed->addressing.destination_port == 5004);
    EXPECT_TRUE(parsed && parsed->packet.header.payload_type() == 99 && parsed->packet.payload.size() == 1152);
}

TEST(rtp_frame, parses_a_packet_of_the_largest_payload_a_frame_carries)
{
    auto const parsed = parse_rtp_frame(eight_channel_frame(1500 - 20 - 8 - RtpHeader::LENGTH));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed && parsed->packet.payload.size() == size_t{1460});
}

TEST(rtp_frame, refuses_a_frame_cut_short_by_a_one_kib_receive_buffer)
{
    // What a NIC writing 1 KiB buffers hands an AF_XDP socket of a larger frame: its first KiB.
    auto const frame = eight_channel_frame(1152);
    EXPECT_FALSE(parse_udp_frame(std::span{frame}.first(1024)).has_value());
}

TEST_MAIN(statusbar_rtp, rtp_frame_test)
