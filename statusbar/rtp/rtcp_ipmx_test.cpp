// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtcp_ipmx.hpp"

#include "statusbar/rtp/rtcp_packet.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace statusbar;
using namespace statusbar::rtp;

namespace {

auto bytes(std::span<uint8_t const> s) -> std::vector<uint8_t>
{
    return {s.begin(), s.end()};
}

// VSF TR-10-3:2024 Section 12, Figure 2: SSRC 2345, an L24/48000/8 stream at 125 us with
// localmac=00-20-FC-32-2F-40, mediaclk sender, block version 3, measuredsamplerate 47952.
constexpr std::array<uint8_t, 148> TR_10_3_SR{
    0x80, 0xC8, 0x00, 0x24,  // V 2, RC 0, PT 200, length 36
    0x00, 0x00, 0x09, 0x29,  // SSRC 2345
    0x63, 0x52, 0xE7, 0x78,  // 1666377592 s
    0x2E, 0x5B, 0x56, 0x02,  // 777737730 ns
    0xF2, 0xA1, 0x34, 0x6F,  // RTP timestamp 4070650991
    0x00, 0x89, 0x56, 0x70,  // 9000560 packets
    0x19, 0xC0, 0x35, 0x00,  // 432026880 octets
    0x58, 0x31, 0x00, 0x1D,  // IPMX tag, length 29
    0x03, 0x00, 0x00, 0x00,  // block version 3
    0x6C, 0x6F, 0x63, 0x61,  // "localmac=00-20-FC-32-2F-40", NUL padded to 64
    0x6C, 0x6D, 0x61, 0x63,  //
    0x3D, 0x30, 0x30, 0x2D,  //
    0x32, 0x30, 0x2D, 0x46,  //
    0x43, 0x2D, 0x33, 0x32,  //
    0x2D, 0x32, 0x46, 0x2D,  //
    0x34, 0x30, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x73, 0x65, 0x6E, 0x64,  // "sender", NUL padded to 12
    0x65, 0x72, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x02, 0x00, 0x08,  // PCM Media Info Block, length 8
    0x00, 0x00, 0xBB, 0x80,  // 48000 Hz
    0x18, 0x08, 0x00, 0x7D,  // 24 bits, 8 channels, 125 us
    0x00, 0x00, 0xBB, 0x50,  // measuredsamplerate 47952
    0x00, 0x00, 0x00, 0x04,  // channel-order string, 4 words
    0x53, 0x4D, 0x50, 0x54,  // "SMPTE2110.(U08)", NUL padded
    0x45, 0x32, 0x31, 0x31,  //
    0x30, 0x2E, 0x28, 0x55,  //
    0x30, 0x38, 0x29, 0x00,  //
};

auto tr_10_3_block() -> IpmxInfoBlock
{
    return {
        .version = 3,
        .ts_refclk = "localmac=00-20-FC-32-2F-40",
        .mediaclk = "sender",
        .pcm =
            IpmxPcmMediaInfo{
                .sampling_rate = 48000,
                .sample_size = 24,
                .channel_count = 8,
                .packet_time_us = 125,
                .measured_sample_rate = 47952,
                .channel_order = "SMPTE2110.(U08)",
            },
    };
}

}  // namespace

TEST(rtcp_ipmx, the_tr_10_3_example_parses)
{
    auto const compound = parse_rtcp(TR_10_3_SR);
    EXPECT_TRUE(compound.has_value());
    EXPECT_TRUE(compound->sr.has_value());
    EXPECT_EQ(compound->sr->ssrc, uint32_t{2345});
    EXPECT_EQ(compound->sr->info.ntp_msw.get(), uint32_t{1666377592});
    EXPECT_EQ(compound->sr->info.ntp_lsw.get(), uint32_t{777737730});
    EXPECT_EQ(compound->sr->info.rtp_timestamp.get(), uint32_t{4070650991});
    EXPECT_EQ(compound->sr->info.packet_count.get(), uint32_t{9000560});
    EXPECT_EQ(compound->sr->info.octet_count.get(), uint32_t{432026880});
    EXPECT_EQ(compound->sr->extension.size(), size_t{120});

    auto const block = parse_ipmx_info_block(compound->sr->extension);
    EXPECT_TRUE(block.has_value());
    EXPECT_TRUE(*block == tr_10_3_block());
}

TEST(rtcp_ipmx, the_tr_10_3_example_is_written_byte_for_byte)
{
    auto const block = tr_10_3_block();
    EXPECT_EQ(ipmx_info_block_size(block), size_t{120});
    std::array<uint8_t, 120> extension{};
    EXPECT_EQ(serialize_ipmx_info_block(block, extension), std::optional<size_t>{120});

    SenderInfo info{};
    info.ntp_msw = 1666377592U;
    info.ntp_lsw = 777737730U;
    info.rtp_timestamp = 4070650991U;
    info.packet_count = 9000560U;
    info.octet_count = 432026880U;
    std::array<uint8_t, 256> out{};
    RtcpWriter writer{out};
    writer.sender_report(2345, info, {}, extension);
    EXPECT_EQ(bytes(writer.bytes()), bytes(TR_10_3_SR));
}

TEST(rtcp_ipmx, a_block_without_a_media_info_block_is_84_bytes)
{
    IpmxInfoBlock const block{.version = 1, .ts_refclk = "ptp=IEEE1588-2008:ec-46-70-ff-fe-10-ff-b0:127", .mediaclk = "direct=0"};
    std::array<uint8_t, 84> out{};
    EXPECT_EQ(serialize_ipmx_info_block(block, out), std::optional<size_t>{84});
    EXPECT_EQ(out[3], uint8_t{20});  // 21 words less one
    auto const parsed = parse_ipmx_info_block(out);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(*parsed == block);
}

TEST(rtcp_ipmx, strings_that_fill_their_fields_need_no_nul)
{
    std::string const refclk(64, 'r');
    IpmxInfoBlock block{.version = 2, .ts_refclk = refclk, .mediaclk = "direct=12345"};
    block.pcm = IpmxPcmMediaInfo{.sampling_rate = 48000, .sample_size = 24, .channel_count = 2, .channel_order = "SMPTE2110.(ST)"};
    std::array<uint8_t, 128> out{};
    auto const size = serialize_ipmx_info_block(block, out);
    EXPECT_EQ(size, std::optional<size_t>{84 + 20 + 16});
    auto const parsed = parse_ipmx_info_block(std::span<uint8_t const>{out}.first(*size));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->ts_refclk, std::string_view{refclk});
    EXPECT_EQ(parsed->mediaclk, std::string_view{"direct=12345"});
    EXPECT_EQ(parsed->pcm->channel_order, std::string_view{"SMPTE2110.(ST)"});
}

TEST(rtcp_ipmx, strings_too_long_or_too_little_room_are_refused)
{
    std::string const refclk(65, 'r');
    std::array<uint8_t, 256> out{};
    EXPECT_FALSE(serialize_ipmx_info_block(IpmxInfoBlock{.ts_refclk = refclk}, out).has_value());
    EXPECT_FALSE(serialize_ipmx_info_block(IpmxInfoBlock{.mediaclk = "direct=123456"}, out).has_value());
    EXPECT_FALSE(serialize_ipmx_info_block(tr_10_3_block(), std::span{out}.first(119)).has_value());
}

TEST(rtcp_ipmx, another_tag_is_not_an_ipmx_block)
{
    auto extension = bytes(std::span<uint8_t const>{TR_10_3_SR}.subspan(28));
    extension[1] = 0x32;
    EXPECT_FALSE(parse_ipmx_info_block(extension).has_value());
    EXPECT_FALSE(parse_ipmx_info_block(std::span<uint8_t const>{}).has_value());
}

TEST(rtcp_ipmx, lengths_past_the_extension_are_refused)
{
    auto const good = bytes(std::span<uint8_t const>{TR_10_3_SR}.subspan(28));
    auto block = good;
    block[3] = 30;  // one word more than there is
    EXPECT_FALSE(parse_ipmx_info_block(block).has_value());
    block = good;
    block[87] = 9;  // the Media Info Block one word longer than the Info Block
    EXPECT_FALSE(parse_ipmx_info_block(block).has_value());
    block = good;
    block[103] = 5;  // the channel-order string longer than its Media Info Block
    EXPECT_FALSE(parse_ipmx_info_block(block).has_value());
    EXPECT_FALSE(parse_ipmx_info_block(std::span<uint8_t const>{good}.first(80)).has_value());
}

TEST(rtcp_ipmx, unknown_media_info_blocks_are_skipped)
{
    // The fixed part, a type 0x7777 block of 2 words, then the TR-10-3 PCM block.
    auto const good = bytes(std::span<uint8_t const>{TR_10_3_SR}.subspan(28));
    std::vector<uint8_t> block{good.begin(), good.begin() + 84};
    block.insert(block.end(), {0x77, 0x77, 0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF});
    block.insert(block.end(), good.begin() + 84, good.end());
    block[3] = 31;
    auto const parsed = parse_ipmx_info_block(block);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->pcm.has_value());
    EXPECT_EQ(parsed->pcm->measured_sample_rate, uint32_t{47952});
}

TEST_MAIN(statusbar_rtp, rtcp_ipmx_test)
