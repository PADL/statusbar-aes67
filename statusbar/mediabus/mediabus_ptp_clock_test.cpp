// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/mediabus/mediabus_ptp_clock.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>

using namespace statusbar::mediabus;

namespace {

// 2026-10-22 in TAI nanoseconds: index arithmetic must stay exact at today's epoch.
constexpr std::uint64_t NOW_NS = 1'792'627'237'000'000'000ULL;

}  // namespace

TEST(mediabus_ptp_clock, supports_only_the_48k_family)
{
    EXPECT_TRUE(PtpMediaClock::supports(48000));
    EXPECT_TRUE(PtpMediaClock::supports(96000));
    EXPECT_TRUE(PtpMediaClock::supports(192000));
    EXPECT_FALSE(PtpMediaClock::supports(44100));
    EXPECT_FALSE(PtpMediaClock::supports(0));
    EXPECT_EQ(PtpMediaClock{48000}.samples_per_tick(), std::uint32_t{6});
    EXPECT_EQ(PtpMediaClock{96000}.samples_per_tick(), std::uint32_t{12});
    EXPECT_EQ(PtpMediaClock{192000}.samples_per_tick(), std::uint32_t{24});
}

TEST(mediabus_ptp_clock, index_is_ptp_time_times_rate)
{
    PtpMediaClock const clock{48000};
    EXPECT_EQ(clock.index_at(0), std::uint64_t{0});
    EXPECT_EQ(clock.index_at(20'833), std::uint64_t{0});  // sample 1 is at 20833.33 ns
    EXPECT_EQ(clock.index_at(20'834), std::uint64_t{1});
    EXPECT_EQ(clock.index_at(1'000'000'000), std::uint64_t{48000});
    EXPECT_EQ(clock.index_at(NOW_NS), (NOW_NS / 1'000'000'000ULL) * 48000ULL);
}

TEST(mediabus_ptp_clock, time_of_inverts_index_at)
{
    for (std::uint32_t const fs : {48000U, 96000U, 192000U}) {
        PtpMediaClock const clock{fs};
        std::uint64_t const base = clock.index_at(NOW_NS);
        for (std::uint64_t n = base; n < base + (3 * fs); n += 7) {
            std::uint64_t const t = clock.time_of(n);
            EXPECT_EQ(clock.index_at(t), n);
            EXPECT_EQ(clock.index_at(t - 1), n - 1);  // time_of is the first ns of sample n
        }
    }
}

TEST(mediabus_ptp_clock, rtp_timestamp_is_the_low_32_bits)
{
    PtpMediaClock const clock{48000};
    std::uint64_t const n = clock.index_at(NOW_NS);
    auto const rtp = static_cast<std::uint32_t>(n);
    EXPECT_EQ(PtpMediaClock::unwrap(rtp, n), n);
    EXPECT_EQ(PtpMediaClock::unwrap(rtp, n + 1000), n);  // reference ahead
    EXPECT_EQ(PtpMediaClock::unwrap(rtp, n - 1000), n);  // reference behind
    EXPECT_EQ(PtpMediaClock::unwrap(rtp + 5, n), n + 5);
    // Across a 32-bit wrap.
    std::uint64_t const wrap = (n | 0xffff'ffffULL) + 1;
    EXPECT_EQ(PtpMediaClock::unwrap(static_cast<std::uint32_t>(wrap + 3), wrap - 3), wrap + 3);
    EXPECT_EQ(PtpMediaClock::unwrap(static_cast<std::uint32_t>(wrap - 3), wrap + 3), wrap - 3);
}
