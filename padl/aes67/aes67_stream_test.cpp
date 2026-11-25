// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/aes67/aes67_error.hpp"
#include "padl/aes67/aes67_receiver.hpp"
#include "padl/aes67/aes67_sender.hpp"
#include "padl/aes67/aes67_socket.hpp"
#include "padl/aes67/aes67_test_support.hpp"
#include "padl/aes67/aes67_udp_transport.hpp"
#include "padl/mediaclock/mediaclock_ptp_clock.hpp"
#include "padl/mediaclock/mediaclock_timebase.hpp"
#include "padl/rtp/rtp_header.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace padl::aes67;
using statusbar::ip::IPv4Address;

TEST(aes67_stream, silence_is_zero_for_linear_pcm_and_the_mbla_word_for_am824)
{
    EXPECT_EQ(silence_word(padl::sdp::Encoding::l16), uint32_t{0});
    EXPECT_EQ(silence_word(padl::sdp::Encoding::l24), uint32_t{0});
    EXPECT_EQ(silence_word(padl::sdp::Encoding::l32), uint32_t{0});
    EXPECT_EQ(silence_word(padl::sdp::Encoding::am824), uint32_t{0x4000'0000});

    for (size_t const size : {size_t{4}, size_t{8}, size_t{12}, size_t{60}, size_t{1440}}) {
        std::vector<uint8_t> bytes(size, 0x55);
        fill_silence(padl::sdp::Encoding::am824, bytes);
        EXPECT_TRUE(all_words_are(bytes, 0x4000'0000));
        std::ranges::fill(bytes, uint8_t{0x55});
        fill_silence(padl::sdp::Encoding::l24, bytes);
        EXPECT_TRUE(std::ranges::all_of(bytes, [](uint8_t b) { return b == 0; }));
    }
}

TEST(aes67_stream, an_sr_class_sends_the_packet_time_of_its_interval)
{
    EXPECT_EQ(srp_frames_per_packet(48000, SrClass::a), uint32_t{6});   // 125 us
    EXPECT_EQ(srp_frames_per_packet(48000, SrClass::b), uint32_t{12});  // 250 us
    EXPECT_EQ(srp_frames_per_packet(96000, SrClass::a), uint32_t{12});
    EXPECT_EQ(srp_frames_per_packet(96000, SrClass::b), uint32_t{24});
    EXPECT_EQ(srp_frames_per_packet(192000, SrClass::a), uint32_t{24});
    EXPECT_EQ(srp_frames_per_packet(44100, SrClass::a), uint32_t{6});   // ~136 us, AES67-2023 Table 2
    EXPECT_EQ(srp_frames_per_packet(44100, SrClass::b), uint32_t{12});  // ~272 us
    EXPECT_EQ(srp_frames_per_packet(8000, SrClass::a), uint32_t{1});    // never none
    EXPECT_EQ(srp_frames_per_packet(0, SrClass::b), uint32_t{1});
}

TEST(aes67_stream, srp_tspecs_count_ip_udp_rtp_the_payload_and_a_spare_octet)
{
    auto const format = [](padl::sdp::Encoding const encoding, uint16_t const channels, uint32_t const rate, uint32_t const fpp) {
        return StreamFormat{.encoding = encoding, .channels = channels, .sample_rate = rate, .frames_per_packet = fpp};
    };
    auto const l24 = padl::sdp::Encoding::l24;
    EXPECT_TRUE(srp_tspec(format(l24, 2, 48000, 6)) == (SrpTSpec{77, 1}));
    EXPECT_TRUE(srp_tspec(format(l24, 8, 48000, 6)) == (SrpTSpec{185, 1}));
    EXPECT_TRUE(srp_tspec(format(padl::sdp::Encoding::l16, 8, 48000, 6)) == (SrpTSpec{137, 1}));
    EXPECT_TRUE(srp_tspec(format(padl::sdp::Encoding::l32, 8, 48000, 6)) == (SrpTSpec{233, 1}));
    EXPECT_TRUE(srp_tspec(format(l24, 16, 96000, 12)) == (SrpTSpec{617, 1}));
    EXPECT_TRUE(srp_tspec(format(l24, 64, 48000, 6)) == (SrpTSpec{1193, 1}));
    EXPECT_TRUE(srp_tspec(format(l24, 8, 48000, 12), SrClass::b) == (SrpTSpec{329, 1}));
    EXPECT_TRUE(srp_tspec(format(l24, 8, 48000, 6), SrClass::b) == (SrpTSpec{185, 2}));
    EXPECT_TRUE(srp_tspec(format(l24, 8, 48000, 48)) == (SrpTSpec{1193, 1}));  // 1 ms: used as it is
    EXPECT_TRUE(srp_tspec(format(l24, 8, 96000, 24), SrClass::b) == (SrpTSpec{617, 1}));
    EXPECT_TRUE(srp_tspec(format(l24, 2, 44100, 6)) == (SrpTSpec{77, 1}));  // 136 us
    EXPECT_TRUE(srp_tspec(format(l24, 2, 44100, 12), SrClass::b) == (SrpTSpec{113, 1}));
    EXPECT_TRUE(srp_tspec(format(l24, 2, 48000, 0)) == SrpTSpec{});
}

TEST(aes67_stream, groups_alias_when_their_low_23_bits_agree)
{
    EXPECT_TRUE(multicast_mac_aliases(IPv4Address{239, 69, 1, 2}, IPv4Address{239, 197, 1, 2}));
    EXPECT_TRUE(multicast_mac_aliases(IPv4Address{239, 69, 1, 2}, IPv4Address{224, 69, 1, 2}));
    EXPECT_TRUE(multicast_mac_aliases(IPv4Address{239, 69, 1, 2}, IPv4Address{239, 69, 1, 2}));
    EXPECT_TRUE(multicast_mac_aliases(IPv4Address{224, 0, 1, 129}, IPv4Address{239, 128, 1, 129}));        // PTP's MAC
    EXPECT_TRUE(multicast_mac_aliases(IPv4Address{239, 255, 255, 255}, IPv4Address{239, 127, 255, 255}));  // SAP's
    EXPECT_FALSE(multicast_mac_aliases(IPv4Address{239, 69, 1, 2}, IPv4Address{239, 69, 1, 3}));
    EXPECT_FALSE(multicast_mac_aliases(IPv4Address{239, 69, 1, 2}, IPv4Address{10, 69, 1, 2}));
}

// named_grandmaster() reads back what Grandmaster::references() writes: IEEE 1588 in a domain, IEEE 802.1AS, either
// traceable; the first reference that names a grandmaster; none from references that name none.
TEST(aes67_stream, named_grandmaster_reads_back_what_references_writes)
{
    for (Grandmaster const grandmaster :
         {GRANDMASTER,
          Grandmaster{.identity = GRANDMASTER.identity, .domain = 5, .traceable = true},
          Grandmaster{.identity = GRANDMASTER.identity, .version = padl::sdp::PtpVersion::ieee802_1as, .traceable = true}}) {
        EXPECT_TRUE(named_grandmaster(grandmaster.references().ptp) == std::optional{grandmaster});
    }
    EXPECT_FALSE(named_grandmaster({}).has_value());
    padl::sdp::ReferenceClocks clocks = padl::sdp::ReferenceClocks::of(padl::sdp::PtpReference{.traceable = true});
    EXPECT_FALSE(named_grandmaster(clocks.ptp).has_value());
    clocks.ptp.push_back(padl::sdp::PtpReference{.version = padl::sdp::PtpVersion::other, .text = "ptp=IEEE1588-2030:x"});
    EXPECT_FALSE(named_grandmaster(clocks.ptp).has_value());
    Grandmaster const second{.identity = *ieee::eui64_from_string("EC-46-70-FF-FE-10-FF-B0"), .domain = 3};
    clocks.ptp.push_back(second.reference());
    clocks.ptp.push_back(GRANDMASTER.reference());
    auto const named = named_grandmaster(clocks.ptp);
    EXPECT_TRUE(named && named->identity == second.identity && named->domain == 3 && named->traceable);
    // Traceable only under its own version: an IEEE 1588 `traceable` says nothing of an IEEE 802.1AS grandmaster.
    padl::sdp::ReferenceClocks gptp = padl::sdp::ReferenceClocks::of(
        Grandmaster{.identity = GRANDMASTER.identity, .version = padl::sdp::PtpVersion::ieee802_1as}.reference());
    gptp.ptp.push_back(padl::sdp::PtpReference{.version = padl::sdp::PtpVersion::ieee1588_2008, .traceable = true});
    auto const untraced = named_grandmaster(gptp.ptp);
    EXPECT_TRUE(untraced && untraced->version == padl::sdp::PtpVersion::ieee802_1as && !untraced->traceable);
}

TEST_MAIN(padl_aes67, aes67_stream_test)
