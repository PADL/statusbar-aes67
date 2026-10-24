// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtcp_time.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>
#include <optional>

using namespace statusbar;
using namespace statusbar::rtp;

namespace {

// TR-10-3 Section 12's SR time: 2022-10-21T18:39:52.777737730 on the PTP timescale.
constexpr uint64_t EXAMPLE_NS = (1666377592ULL * NS_PER_SECOND) + 777737730ULL;

}  // namespace

TEST(rtcp_time, ptp_truncated_time_splits_seconds_and_nanoseconds)
{
    constexpr auto truncated = ptp_truncated_from_ns(EXAMPLE_NS);
    static_assert(truncated.seconds == 1666377592U && truncated.nanoseconds == 777737730U);
    EXPECT_EQ(truncated.seconds, uint32_t{1666377592});
    EXPECT_EQ(truncated.nanoseconds, uint32_t{777737730});
}

TEST(rtcp_time, ptp_truncated_time_round_trips_about_a_reference)
{
    auto const truncated = ptp_truncated_from_ns(EXAMPLE_NS);
    EXPECT_EQ(ns_from_ptp_truncated(truncated, EXAMPLE_NS), std::optional<uint64_t>{EXAMPLE_NS});
    EXPECT_EQ(ns_from_ptp_truncated(truncated, EXAMPLE_NS + (3600 * NS_PER_SECOND)), std::optional<uint64_t>{EXAMPLE_NS});
    EXPECT_EQ(ns_from_ptp_truncated(truncated, EXAMPLE_NS - (3600 * NS_PER_SECOND)), std::optional<uint64_t>{EXAMPLE_NS});
}

TEST(rtcp_time, the_2106_seconds_wrap_is_crossed_both_ways)
{
    constexpr uint64_t WRAP_NS = (1ULL << 32U) * NS_PER_SECOND;
    uint64_t const before = WRAP_NS - (5 * NS_PER_SECOND) + 1;
    uint64_t const after = WRAP_NS + (5 * NS_PER_SECOND) + 2;
    EXPECT_EQ(ptp_truncated_from_ns(after).seconds, uint32_t{5});
    EXPECT_EQ(ns_from_ptp_truncated(ptp_truncated_from_ns(after), before), std::optional<uint64_t>{after});
    EXPECT_EQ(ns_from_ptp_truncated(ptp_truncated_from_ns(before), after), std::optional<uint64_t>{before});
}

TEST(rtcp_time, nanoseconds_of_a_second_or_more_are_refused)
{
    EXPECT_FALSE(ns_from_ptp_truncated({.seconds = 1, .nanoseconds = 1'000'000'000}, EXAMPLE_NS).has_value());
    // A time before the epoch, about a reference near it.
    EXPECT_FALSE(ns_from_ptp_truncated({.seconds = 0xFFFF'FFFF, .nanoseconds = 0}, 0).has_value());
}

TEST(rtcp_time, lsr_is_the_middle_of_the_time_field)
{
    static_assert(lsr_of(0x1234'5678, 0x9ABC'DEF0) == 0x5678'9ABCU);
    EXPECT_EQ(lsr_of(1666377592, 777737730), uint32_t{((1666377592U & 0xFFFFU) << 16U) | (777737730U >> 16U)});
}

TEST(rtcp_time, dlsr_is_in_65536ths_of_a_second)
{
    static_assert(dlsr_from_ns(NS_PER_SECOND) == 65536U);
    EXPECT_EQ(dlsr_from_ns(500'000'000), uint32_t{32768});
    EXPECT_EQ(dlsr_from_ns(0), uint32_t{0});
    EXPECT_EQ(dlsr_from_ns(70'000 * NS_PER_SECOND), uint32_t{0xFFFF'FFFF});
    EXPECT_EQ(ns_from_dlsr(65536), NS_PER_SECOND);
    EXPECT_EQ(ns_from_dlsr(32768), uint64_t{500'000'000});
}

TEST(rtcp_time, the_round_trip_is_arrival_less_send_less_the_receivers_hold)
{
    // An SR at EXAMPLE_NS; the receiver holds it 250 ms; the RR arrives 250.3 ms after the SR.
    uint32_t const dlsr = dlsr_from_ns(250'000'000);
    auto const rtt = rtt_ns(EXAMPLE_NS + 250'300'000, EXAMPLE_NS, dlsr);
    EXPECT_TRUE(rtt.has_value());
    // DLSR's resolution is 15.26 us: within one unit of 300 us.
    EXPECT_TRUE(*rtt >= 300'000);
    EXPECT_TRUE(*rtt < 300'000 + 15'259);
    EXPECT_FALSE(rtt_ns(EXAMPLE_NS + 100'000'000, EXAMPLE_NS, dlsr).has_value());
    EXPECT_FALSE(rtt_ns(EXAMPLE_NS - 1, EXAMPLE_NS, 0).has_value());
}

TEST_MAIN(statusbar_rtp, rtcp_time_test)
