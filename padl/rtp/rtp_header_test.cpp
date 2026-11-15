// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/rtp/rtp_header.hpp"

#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <vector>

using namespace statusbar;
using namespace padl::rtp;

namespace {

// An AES67 packet: version 2, PT 96, seq 0x1234, ts 0x89ABCDEF, SSRC 0x01020304,
// then 4 bytes of L24 (not a whole frame, but the parser does not care).
constexpr std::array<uint8_t, 16> BASIC{
    0x80, 0x60, 0x12, 0x34, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC, 0xDD};

auto bytes(std::span<uint8_t const> s) -> std::vector<uint8_t>
{
    return {s.begin(), s.end()};
}

}  // namespace

TEST(rtp_header, parses_an_aes67_packet)
{
    auto const packet = parse_rtp(BASIC);
    EXPECT_TRUE(packet.has_value());
    EXPECT_EQ(packet->header.version(), uint8_t{2});
    EXPECT_FALSE(packet->header.marker());
    EXPECT_EQ(packet->header.payload_type(), uint8_t{96});
    EXPECT_EQ(packet->header.sequence.get(), uint16_t{0x1234});
    EXPECT_EQ(packet->header.timestamp.get(), uint32_t{0x89ABCDEF});
    EXPECT_EQ(packet->header.ssrc.get(), uint32_t{0x01020304});
    EXPECT_EQ(bytes(packet->payload), (std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD}));
}

TEST(rtp_header, skips_csrcs_and_an_extension_and_strips_padding)
{
    std::vector<uint8_t> const datagram{0xB1, 0xE0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
                                        0x00, 0x00, 0x00, 0x03,                          // V=2 P X CC=1, M, PT 96
                                        0x11, 0x22, 0x33, 0x44,                          // CSRC
                                        0xBE, 0xDE, 0x00, 0x01, 0x55, 0x66, 0x77, 0x88,  // extension: 1 word
                                        0xAA, 0xBB,                                      // payload
                                        0x00, 0x00, 0x03};                               // 3 bytes of padding
    auto const packet = parse_rtp(datagram);
    EXPECT_TRUE(packet.has_value());
    EXPECT_TRUE(packet->header.marker());
    EXPECT_EQ(packet->header.csrc_count(), uint8_t{1});
    EXPECT_EQ(bytes(packet->payload), (std::vector<uint8_t>{0xAA, 0xBB}));
}

TEST(rtp_header, refuses_malformed_packets)
{
    EXPECT_FALSE(parse_rtp(std::span{BASIC}.first(11)).has_value());  // short header

    auto version1 = bytes(BASIC);
    version1[0] = 0x40;
    EXPECT_FALSE(parse_rtp(version1).has_value());

    auto missing_csrc = bytes(BASIC);
    missing_csrc[0] = 0x85;  // five CSRCs claimed, four payload bytes present
    EXPECT_FALSE(parse_rtp(missing_csrc).has_value());

    auto long_extension = bytes(BASIC);
    long_extension[0] = 0x90;  // extension header would need 4 bytes + 0xCCDD words
    EXPECT_FALSE(parse_rtp(long_extension).has_value());

    auto zero_padding = bytes(BASIC);
    zero_padding[0] = 0xA0;
    zero_padding.back() = 0x00;
    EXPECT_FALSE(parse_rtp(zero_padding).has_value());

    auto over_padding = bytes(BASIC);
    over_padding[0] = 0xA0;
    over_padding.back() = 0x05;  // more than the 4 bytes after the header
    EXPECT_FALSE(parse_rtp(over_padding).has_value());
}

TEST(rtp_header, serializes_what_it_parses)
{
    RtpHeader header{};
    header.init(96, 0x1234, 0x89ABCDEF, 0x01020304);
    std::array<uint8_t const, 4> const payload{0xAA, 0xBB, 0xCC, 0xDD};
    std::array<uint8_t, 32> datagram{};
    auto const length = serialize_rtp(header, payload, datagram);
    EXPECT_TRUE(length.has_value());
    EXPECT_EQ(*length, BASIC.size());
    EXPECT_EQ(bytes(std::span{datagram}.first(*length)), bytes(BASIC));

    std::array<uint8_t, 15> small{};
    EXPECT_FALSE(serialize_rtp(header, payload, small).has_value());
}

TEST(rtp_header, a_marked_packet_of_any_payload_type_is_rtp)
{
    // marker + PT 72 is 200, an RTCP SR type; without RTCP on the port it is RTP
    auto marked = bytes(BASIC);
    marked[1] = 200;
    auto const packet = parse_rtp(marked);
    EXPECT_TRUE(packet.has_value());
    EXPECT_TRUE(packet->header.marker());
    EXPECT_EQ(packet->header.payload_type(), uint8_t{72});
    marked[1] = 0xFF;  // marker + PT 127
    EXPECT_EQ(parse_rtp(marked)->header.payload_type(), uint8_t{127});
}

TEST(rtp_header, padding_may_consume_the_whole_payload)
{
    auto all_padding = bytes(BASIC);
    all_padding[0] = 0xA0;
    all_padding.back() = 0x04;
    auto const packet = parse_rtp(all_padding);
    EXPECT_TRUE(packet.has_value());
    EXPECT_TRUE(packet->payload.empty());
}

TEST(rtp_header, serializes_a_fixed_header_only)
{
    std::vector<uint8_t> const packet{0xB1, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
                                      0x11, 0x22, 0x33, 0x44, 0xBE, 0xDE, 0x00, 0x00, 0xAA, 0x00, 0x00, 0x03};
    auto parsed = parse_rtp(packet);  // its payload is a view into `packet`, which must outlive it
    EXPECT_TRUE(parsed.has_value());
    std::array<uint8_t, 32> datagram{};
    auto const length = serialize_rtp(parsed->header, parsed->payload, datagram);
    EXPECT_TRUE(length.has_value());
    EXPECT_EQ(*length, size_t{13});
    EXPECT_EQ(datagram[0], uint8_t{0x80});  // P, X and CC cleared
    auto const reparsed = parse_rtp(std::span{datagram}.first(*length));
    EXPECT_TRUE(reparsed.has_value());
    EXPECT_EQ(bytes(reparsed->payload), (std::vector<uint8_t>{0xAA}));
}

TEST_MAIN(padl_rtp, rtp_header_test)
