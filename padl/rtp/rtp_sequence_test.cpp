// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/rtp/rtp_sequence.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>

using namespace statusbar;
using namespace padl::rtp;

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
    EXPECT_EQ(seq.expected(), uint32_t{3});
    EXPECT_EQ(seq.received(), uint32_t{4});  // RFC 3550: duplicates count, so loss can be negative
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

TEST(rtp_sequence, figures_are_zero_on_probation)
{
    RtpSequence seq;
    EXPECT_FALSE(seq.update(100));
    EXPECT_FALSE(seq.update(50));  // a reorder or restart while on probation
    EXPECT_EQ(seq.expected(), uint32_t{0});
    EXPECT_EQ(seq.received(), uint32_t{0});
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

TEST_MAIN(padl_rtp, rtp_sequence_test)
