// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtcp_packet.hpp"

#include "statusbar/rtp/rtp_error.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace statusbar;
using namespace statusbar::rtp;

namespace {

auto bytes(std::span<uint8_t const> s) -> std::vector<uint8_t>
{
    return {s.begin(), s.end()};
}

// RFC 3550 Sections 6.4.1 and 6.5 by hand: an SR from 0x11223344 with no blocks, then SDES with
// CNAME "a@10.0.0.1" (10 bytes: 4 + 2 + 10 + 1 null = 17, padded to 20).
constexpr std::array<uint8_t, 52> SR_SDES{
    0x80, 0xC8, 0x00, 0x06,                          // SR, RC 0, 7 words
    0x11, 0x22, 0x33, 0x44,                          // SSRC
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,  // time field
    0x00, 0x00, 0x00, 0x03,                          // RTP timestamp
    0x00, 0x00, 0x00, 0x04,                          // packets
    0x00, 0x00, 0x00, 0x05,                          // octets
    0x81, 0xCA, 0x00, 0x05,                          // SDES, SC 1, 6 words
    0x11, 0x22, 0x33, 0x44,                          // chunk SSRC
    0x01, 0x0A, 'a',  '@',  '1',  '0',  '.',  '0',   // CNAME
    '.',  '0',  '.',  '1',  0x00, 0x00, 0x00, 0x00,  // end, padding
};

// An RR from 0x0A0B0C0D with one block about 0x11223344, then SDES CNAME "r" (4 + 2 + 1 + 1 = 8).
constexpr std::array<uint8_t, 44> RR_SDES{
    0x81, 0xC9, 0x00, 0x07,  // RR, RC 1, 8 words
    0x0A, 0x0B, 0x0C, 0x0D,  // SSRC
    0x11, 0x22, 0x33, 0x44,  // block: source
    0x40, 0xFF, 0xFF, 0xFE,  // fraction 64/256, cumulative -2
    0x00, 0x01, 0x00, 0x10,  // extended highest sequence
    0x00, 0x00, 0x00, 0x2A,  // jitter
    0x00, 0x01, 0x00, 0x00,  // LSR
    0x00, 0x00, 0x80, 0x00,  // DLSR
    0x81, 0xCA, 0x00, 0x02,  // SDES, SC 1, 3 words
    0x0A, 0x0B, 0x0C, 0x0D,  // chunk SSRC
    0x01, 0x01, 'r',  0x00,  // CNAME, end
};

}  // namespace

TEST(rtcp_packet, parses_an_sr_and_sdes_compound)
{
    auto const compound = parse_rtcp(SR_SDES);
    EXPECT_TRUE(compound.has_value());
    EXPECT_EQ(compound->packets, size_t{2});
    EXPECT_TRUE(compound->sr.has_value());
    EXPECT_FALSE(compound->rr.has_value());
    EXPECT_EQ(compound->sr->ssrc, uint32_t{0x11223344});
    EXPECT_EQ(compound->sr->info.ntp_msw.get(), uint32_t{1});
    EXPECT_EQ(compound->sr->info.ntp_lsw.get(), uint32_t{2});
    EXPECT_EQ(compound->sr->info.rtp_timestamp.get(), uint32_t{3});
    EXPECT_EQ(compound->sr->info.packet_count.get(), uint32_t{4});
    EXPECT_EQ(compound->sr->info.octet_count.get(), uint32_t{5});
    EXPECT_EQ(compound->sr->blocks.count(), size_t{0});
    EXPECT_TRUE(compound->sr->extension.empty());
    EXPECT_TRUE(compound->sdes.has_value());
    EXPECT_EQ(compound->sdes->ssrc, uint32_t{0x11223344});
    EXPECT_EQ(compound->sdes->cname, std::string_view{"a@10.0.0.1"});
    EXPECT_TRUE(compound->sdes->name.empty());
}

TEST(rtcp_packet, writes_an_sr_and_sdes_compound_byte_for_byte)
{
    SenderInfo info{};
    info.ntp_msw = 1U;
    info.ntp_lsw = 2U;
    info.rtp_timestamp = 3U;
    info.packet_count = 4U;
    info.octet_count = 5U;
    std::array<uint8_t, 128> out{};
    RtcpWriter writer{out};
    writer.sender_report(0x11223344, info).sdes(0x11223344, "a@10.0.0.1");
    EXPECT_TRUE(static_cast<bool>(writer));
    EXPECT_EQ(bytes(writer.bytes()), bytes(SR_SDES));
}

TEST(rtcp_packet, parses_an_rr_and_its_report_block)
{
    auto const compound = parse_rtcp(RR_SDES);
    EXPECT_TRUE(compound.has_value());
    EXPECT_TRUE(compound->rr.has_value());
    EXPECT_EQ(compound->rr->ssrc, uint32_t{0x0A0B0C0D});
    EXPECT_EQ(compound->rr->blocks.count(), size_t{1});
    auto const block = compound->rr->blocks.find(0x11223344);
    EXPECT_TRUE(block.has_value());
    EXPECT_EQ(block->fraction_lost(), uint8_t{64});
    EXPECT_EQ(block->cumulative_lost(), int32_t{-2});
    EXPECT_EQ(block->extended_highest_seq.get(), uint32_t{0x00010010});
    EXPECT_EQ(block->jitter.get(), uint32_t{42});
    EXPECT_EQ(block->lsr.get(), uint32_t{0x00010000});
    EXPECT_EQ(block->dlsr.get(), uint32_t{0x8000});
    EXPECT_FALSE(compound->rr->blocks.find(0x99).has_value());
    EXPECT_EQ(compound->sdes->cname, std::string_view{"r"});
}

TEST(rtcp_packet, writes_an_rr_and_sdes_compound_byte_for_byte)
{
    ReportBlock block{};
    block.ssrc = 0x11223344U;
    block.set_loss(64, -2);
    block.extended_highest_seq = 0x00010010U;
    block.jitter = 42U;
    block.lsr = 0x00010000U;
    block.dlsr = 0x8000U;
    std::array<uint8_t, 128> out{};
    RtcpWriter writer{out};
    writer.receiver_report(0x0A0B0C0D, std::span{&block, 1}).sdes(0x0A0B0C0D, "r");
    EXPECT_EQ(bytes(writer.bytes()), bytes(RR_SDES));
}

TEST(rtcp_packet, cumulative_loss_is_clamped_to_24_signed_bits)
{
    ReportBlock block{};
    block.set_loss(1, 0x0100'0000);
    EXPECT_EQ(block.cumulative_lost(), int32_t{0x7F'FFFF});
    block.set_loss(1, -0x0100'0000);
    EXPECT_EQ(block.cumulative_lost(), int32_t{-0x80'0000});
    EXPECT_EQ(block.fraction_lost(), uint8_t{1});
}

TEST(rtcp_packet, a_bye_round_trips_with_its_reason)
{
    std::array<uint8_t, 128> out{};
    RtcpWriter writer{out};
    writer.receiver_report(7).sdes(7, "x@y").bye(7, "gone");
    EXPECT_TRUE(static_cast<bool>(writer));
    // RR 8, SDES 4 + 4 + 2 + 3 + 1 = 14 -> 16, BYE 4 + 4 + 1 + 4 = 13 -> 16.
    EXPECT_EQ(writer.bytes().size(), size_t{40});
    auto const compound = parse_rtcp(writer.bytes());
    EXPECT_TRUE(compound.has_value());
    EXPECT_EQ(compound->packets, size_t{3});
    EXPECT_TRUE(compound->bye.has_value());
    EXPECT_EQ(compound->bye->count(), size_t{1});
    EXPECT_TRUE(compound->bye->contains(7));
    EXPECT_FALSE(compound->bye->contains(8));
    EXPECT_EQ(compound->bye->reason, std::string_view{"gone"});
}

TEST(rtcp_packet, an_sr_extension_is_returned_after_the_report_blocks)
{
    ReportBlock block{};
    block.ssrc = 9U;
    std::array<uint8_t, 8> const extension{0x58, 0x31, 0x00, 0x01, 1, 2, 3, 4};
    std::array<uint8_t, 128> out{};
    RtcpWriter writer{out};
    writer.sender_report(5, SenderInfo{}, std::span{&block, 1}, extension);
    auto const compound = parse_rtcp(writer.bytes());
    EXPECT_TRUE(compound.has_value());
    EXPECT_EQ(compound->sr->blocks.count(), size_t{1});
    EXPECT_EQ(compound->sr->blocks.at(0).ssrc.get(), uint32_t{9});
    EXPECT_EQ(bytes(compound->sr->extension), bytes(extension));
}

TEST(rtcp_packet, padding_on_the_last_packet_is_removed)
{
    // An RR, then a BYE with 4 octets of padding (P set, the last octet counting them).
    constexpr std::array<uint8_t, 20> padded{0x80, 0xC9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0xA1, 0xCB,
                                             0x00, 0x02, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x04};
    auto const compound = parse_rtcp(padded);
    EXPECT_TRUE(compound.has_value());
    EXPECT_TRUE(compound->bye.has_value());
    EXPECT_TRUE(compound->bye->reason.empty());
    EXPECT_TRUE(compound->bye->contains(7));
}

TEST(rtcp_packet, padding_on_the_first_or_an_inner_packet_is_refused)
{
    constexpr std::array<uint8_t, 12> alone{0xA0, 0xC9, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x04};
    EXPECT_FALSE(parse_rtcp(alone).has_value());
    constexpr std::array<uint8_t, 16> padded{
        0xA0, 0xC9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x81, 0xCB, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07};
    EXPECT_FALSE(parse_rtcp(padded).has_value());
}

TEST(rtcp_packet, truncated_and_malformed_packets_are_refused)
{
    EXPECT_FALSE(parse_rtcp(std::span<uint8_t const>{}).has_value());
    EXPECT_FALSE(parse_rtcp(std::span<uint8_t const>{SR_SDES}.first(50)).has_value());  // not whole words
    EXPECT_FALSE(parse_rtcp(std::span<uint8_t const>{SR_SDES}.first(48)).has_value());  // SDES runs past the end
    EXPECT_FALSE(parse_rtcp(std::span<uint8_t const>{SR_SDES}.first(24)).has_value());  // SR runs past the end
    auto version = SR_SDES;
    version[0] = 0x40;  // version 1
    EXPECT_FALSE(parse_rtcp(version).has_value());
    auto blocks = SR_SDES;
    blocks[0] = 0x81;  // RC 1, but no room for the block
    EXPECT_FALSE(parse_rtcp(blocks).has_value());
    auto unterminated = SR_SDES;
    unterminated[37] = 0x0F;  // CNAME length past the chunk's end
    EXPECT_FALSE(parse_rtcp(unterminated).has_value());
}

TEST(rtcp_packet, the_first_packet_must_be_a_report)
{
    constexpr std::array<uint8_t, 8> bye{0x81, 0xCB, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07};
    EXPECT_FALSE(parse_rtcp(bye).has_value());
    // An RTP packet sent to the RTCP port: PT 96 with the marker reads as type 224.
    constexpr std::array<uint8_t, 12> rtp{0x80, 0xE0, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_FALSE(parse_rtcp(rtp).has_value());
}

TEST(rtcp_packet, unknown_packet_types_are_skipped)
{
    // An RR, then an APP packet (204) of 3 words, then an XR (207) of 2.
    constexpr std::array<uint8_t, 28> mixed{0x80, 0xC9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x80, 0xCC, 0x00, 0x02, 0x00, 0x00,
                                            0x00, 0x07, 'n',  'a',  'm',  'e',  0x80, 0xCF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07};
    auto const compound = parse_rtcp(mixed);
    EXPECT_TRUE(compound.has_value());
    EXPECT_EQ(compound->packets, size_t{3});
    EXPECT_TRUE(compound->rr.has_value());
    EXPECT_FALSE(compound->sdes.has_value());
}

TEST(rtcp_packet, the_writer_refuses_what_does_not_fit)
{
    std::array<uint8_t, 30> out{};
    RtcpWriter writer{out};
    writer.sender_report(1, SenderInfo{});  // 28 bytes
    EXPECT_TRUE(static_cast<bool>(writer));
    writer.sdes(1, "c");  // 12 more: no room
    EXPECT_FALSE(static_cast<bool>(writer));
    EXPECT_TRUE(writer.status().error() == RtpError::no_room);
    writer.bye(1);  // sticky: nothing more written
    EXPECT_EQ(writer.bytes().size(), size_t{28});

    std::array<uint8_t, 64> room{};
    RtcpWriter unaligned{room};
    std::array<uint8_t, 3> const extension{};
    unaligned.sender_report(1, SenderInfo{}, {}, extension);
    EXPECT_TRUE(unaligned.status().error() == RtpError::unaligned_block);
    EXPECT_TRUE(unaligned.bytes().empty());
}

TEST(rtcp_packet, an_sdes_chunk_that_fills_its_words_still_ends_with_a_null_word)
{
    // 4 + 2 + 2 = 8 bytes of chunk: the list's terminating null needs another whole word.
    std::array<uint8_t, 64> out{};
    RtcpWriter writer{out};
    writer.receiver_report(1).sdes(1, "ab");
    EXPECT_EQ(writer.bytes().size(), size_t{8 + 16});
    auto const compound = parse_rtcp(writer.bytes());
    EXPECT_TRUE(compound.has_value());
    EXPECT_EQ(compound->sdes->cname, std::string_view{"ab"});
}

TEST(rtcp_packet, sdes_carries_a_name_after_the_cname)
{
    std::array<uint8_t, 64> out{};
    RtcpWriter writer{out};
    writer.receiver_report(1).sdes(1, "id@10.0.0.2", "Stage left");
    auto const compound = parse_rtcp(writer.bytes());
    EXPECT_TRUE(compound.has_value());
    EXPECT_EQ(compound->sdes->cname, std::string_view{"id@10.0.0.2"});
    EXPECT_EQ(compound->sdes->name, std::string_view{"Stage left"});
}

TEST_MAIN(statusbar_rtp, rtcp_packet_test)
