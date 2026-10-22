// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtp_header.hpp"
#include "statusbar/rtp/rtp_sequence.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <vector>

using namespace statusbar;
using namespace statusbar::rtp;

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

TEST(rtp_sequence, a_source_is_validated_after_two_in_order_packets)
{
    RtpSequence seq;
    EXPECT_FALSE(seq.update(100));
    EXPECT_FALSE(seq.validated());
    EXPECT_TRUE(seq.update(101));
    EXPECT_TRUE(seq.validated());
    EXPECT_TRUE(seq.update(102));
    EXPECT_EQ(seq.expected(), uint32_t{2});
    EXPECT_EQ(seq.received(), uint32_t{2});
    EXPECT_EQ(seq.lost(), int64_t{0});
}

TEST(rtp_sequence, probation_restarts_on_a_gap)
{
    RtpSequence seq;
    EXPECT_FALSE(seq.update(100));
    EXPECT_FALSE(seq.update(105));  // out of order while on probation
    EXPECT_FALSE(seq.validated());
    EXPECT_TRUE(seq.update(106));
    EXPECT_TRUE(seq.validated());
}

TEST(rtp_sequence, counts_losses_and_wraps)
{
    RtpSequence seq;
    seq.update(65534);
    seq.update(65535);
    EXPECT_TRUE(seq.update(0));  // wrap
    EXPECT_TRUE(seq.update(3));  // 1 and 2 lost
    EXPECT_EQ(seq.extended_max(), uint32_t{65536 + 3});
    EXPECT_EQ(seq.expected(), uint32_t{5});
    EXPECT_EQ(seq.received(), uint32_t{3});
    EXPECT_EQ(seq.lost(), int64_t{2});
}

TEST(rtp_sequence, late_and_duplicate_packets_keep_the_high_water_mark)
{
    RtpSequence seq;
    seq.update(10);
    seq.update(11);
    seq.update(13);
    EXPECT_TRUE(seq.update(12));  // late
    EXPECT_TRUE(seq.update(13));  // duplicate
    EXPECT_EQ(seq.extended_max(), uint32_t{13});
    EXPECT_EQ(seq.lost(), int64_t{-1});  // RFC 3550: duplicates can make loss negative
}

TEST(rtp_sequence, a_jump_is_believed_only_when_confirmed)
{
    RtpSequence seq;
    seq.update(10);
    seq.update(11);
    EXPECT_FALSE(seq.update(40'000));  // the sender restarted, or a stray packet
    EXPECT_EQ(seq.extended_max(), uint32_t{11});
    EXPECT_TRUE(seq.update(12));  // stray: carry on
    EXPECT_FALSE(seq.update(50'000));
    EXPECT_TRUE(seq.update(50'001));  // confirmed: resynchronise
    EXPECT_EQ(seq.extended_max(), uint32_t{50'001});
    EXPECT_EQ(seq.expected(), uint32_t{1});
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
    auto parsed = parse_rtp(std::vector<uint8_t>{0xB1, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
                                                 0x11, 0x22, 0x33, 0x44, 0xBE, 0xDE, 0x00, 0x00, 0xAA, 0x00, 0x00, 0x03});
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

TEST(rtp_sequence, figures_are_zero_on_probation)
{
    RtpSequence seq;
    EXPECT_FALSE(seq.update(100));
    EXPECT_FALSE(seq.update(50));  // a reorder or restart while on probation
    EXPECT_EQ(seq.expected(), uint32_t{0});
    EXPECT_EQ(seq.lost(), int64_t{0});
    EXPECT_EQ(seq.report().cumulative_lost, int32_t{0});
}

TEST(rtp_sequence, probation_across_a_wrap)
{
    RtpSequence seq;
    EXPECT_FALSE(seq.update(65535));
    EXPECT_TRUE(seq.update(0));
    EXPECT_TRUE(seq.update(1));
    EXPECT_EQ(seq.expected(), uint32_t{2});
}

TEST(rtp_sequence, the_misorder_boundary)
{
    RtpSequence seq;
    seq.update(1000);
    seq.update(1001);
    // Up to 99 behind the high-water mark is late; 100 behind is a jump.
    EXPECT_TRUE(seq.update(static_cast<uint16_t>(1001 - RtpSequence::MAX_MISORDER + 1)));
    EXPECT_FALSE(seq.update(static_cast<uint16_t>(1001 - RtpSequence::MAX_MISORDER)));
    // 2999 ahead is in order; 3000 ahead is a jump.
    EXPECT_FALSE(seq.update(static_cast<uint16_t>(1001 + RtpSequence::MAX_DROPOUT)));
    EXPECT_TRUE(seq.update(static_cast<uint16_t>(1001 + RtpSequence::MAX_DROPOUT - 1)));
    EXPECT_EQ(seq.extended_max(), uint32_t{1001 + RtpSequence::MAX_DROPOUT - 1});
}

TEST(rtp_sequence, reports_interval_loss)
{
    RtpSequence seq;
    seq.update(0);
    seq.update(1);  // validated: expected 1, received 1
    for (uint16_t s = 2; s < 9; ++s) {
        if (s != 4 && s != 6) {
            seq.update(s);
        }
    }
    // Since validation: expected 8 (1..8), received 6.
    auto const first = seq.report();
    EXPECT_EQ(first.fraction_lost, uint8_t{64});  // 2/8
    EXPECT_EQ(first.cumulative_lost, int32_t{2});
    seq.update(9);
    seq.update(10);
    auto const second = seq.report();
    EXPECT_EQ(second.fraction_lost, uint8_t{0});
    EXPECT_EQ(second.cumulative_lost, int32_t{2});
}

TEST_MAIN(statusbar_rtp, rtp_test)
