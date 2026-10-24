// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtcp_interval.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>
#include <limits>
#include <numbers>

using namespace statusbar;
using namespace statusbar::rtp;

namespace {

/// Returns `value` every call: a draw of value / 2^32.
struct FixedRng
{
    using result_type = uint32_t;
    static constexpr auto min() noexcept -> uint32_t { return 0; }
    static constexpr auto max() noexcept -> uint32_t { return std::numeric_limits<uint32_t>::max(); }
    auto operator()() noexcept -> uint32_t { return value; }
    uint32_t value{0};
};

constexpr uint64_t SECOND = 1'000'000'000;
constexpr double COMPENSATION = std::numbers::e - 1.5;

}  // namespace

TEST(rtcp_interval, the_first_report_waits_half_the_minimum)
{
    RtcpIntervalParams const p{.minimum_ns = SECOND, .initial = true};
    EXPECT_EQ(deterministic_rtcp_interval(p), SECOND / 2);
    FixedRng middle{.value = 0x8000'0000};  // a draw of one half: exactly Td / 1.21828
    auto const t = rtcp_interval(p, middle);
    EXPECT_EQ(t, static_cast<uint64_t>(static_cast<double>(SECOND) / 2.0 / COMPENSATION));
}

TEST(rtcp_interval, steady_intervals_span_half_to_one_and_a_half_times_the_minimum)
{
    RtcpIntervalParams const p{.minimum_ns = SECOND};
    FixedRng low{.value = 0};
    FixedRng high{.value = 0xFFFF'FFFF};
    auto const lowest = rtcp_interval(p, low);
    auto const highest = rtcp_interval(p, high);
    EXPECT_EQ(lowest, static_cast<uint64_t>(0.5 * static_cast<double>(SECOND) / COMPENSATION));
    EXPECT_TRUE(highest < static_cast<uint64_t>(1.5 * static_cast<double>(SECOND) / COMPENSATION));
    EXPECT_TRUE(highest > static_cast<uint64_t>(1.4999 * static_cast<double>(SECOND) / COMPENSATION));
}

TEST(rtcp_interval, the_minimum_binds_while_the_bandwidth_share_is_smaller)
{
    // One sender of a 2.6 Mbit/s stream among 9 members, 180-octet reports: 5 % is 16250 octets/s,
    // the sender's quarter share 4062.5, so its own 180 octets take 44 ms, under the 1 s minimum.
    RtcpIntervalParams p{
        .minimum_ns = SECOND,
        .members = 9,
        .senders = 1,
        .we_sent = true,
        .rtcp_bandwidth = 16250.0,
        .average_size = 180.0,
    };
    EXPECT_EQ(deterministic_rtcp_interval(p), SECOND);
    // A thin session: 50 octets/s for RTCP makes the sender's share 180 / 12.5 = 14.4 s.
    p.rtcp_bandwidth = 50.0;
    EXPECT_EQ(deterministic_rtcp_interval(p), uint64_t{14'400'000'000});
    // A receiver of the same session shares three quarters among the 8 others: 180 * 8 / 37.5.
    p.we_sent = false;
    EXPECT_EQ(deterministic_rtcp_interval(p), uint64_t{38'400'000'000});
}

TEST(rtcp_interval, with_many_senders_all_members_share_the_bandwidth)
{
    RtcpIntervalParams const p{
        .minimum_ns = SECOND, .members = 4, .senders = 2, .we_sent = true, .rtcp_bandwidth = 100.0, .average_size = 100.0};
    EXPECT_EQ(deterministic_rtcp_interval(p), 4 * SECOND);
}

TEST(rtcp_interval, a_vanishing_bandwidth_is_held_to_a_day)
{
    RtcpIntervalParams const p{.minimum_ns = SECOND, .rtcp_bandwidth = 1e-12, .average_size = 1e6};
    EXPECT_EQ(deterministic_rtcp_interval(p), 86'400 * SECOND);
}

TEST(rtcp_interval, a_64_bit_draw_stays_within_one_and_a_half)
{
    struct MaxRng
    {
        using result_type = uint64_t;
        static constexpr auto min() noexcept -> uint64_t { return 0; }
        static constexpr auto max() noexcept -> uint64_t { return std::numeric_limits<uint64_t>::max(); }
        auto operator()() noexcept -> uint64_t { return max(); }
    } rng;
    RtcpIntervalParams const p{.minimum_ns = SECOND};
    EXPECT_TRUE(rtcp_interval(p, rng) <= static_cast<uint64_t>(1.5 * static_cast<double>(SECOND) / COMPENSATION));
}

TEST_MAIN(statusbar_rtp, rtcp_interval_test)
