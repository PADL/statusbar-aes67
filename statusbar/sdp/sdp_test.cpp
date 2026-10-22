// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/sdp/sdp.hpp"

#include "statusbar/test/test.hpp"

#include <string>
#include <string_view>

using namespace statusbar;
using namespace statusbar::sdp;

namespace {

// Shaped like a Dante device's AES67 announcement: session-level connection,
// media-level clocks, a 125 us packet time written as 0.125.
constexpr std::string_view DANTE = "v=0\r\n"
                                   "o=- 1311738121 1311738121 IN IP4 192.168.1.20\r\n"
                                   "s=AVIO-Bluetooth : 2\r\n"
                                   "c=IN IP4 239.69.161.58/32\r\n"
                                   "t=0 0\r\n"
                                   "a=keywds:Dante\r\n"
                                   "m=audio 5004 RTP/AVP 97\r\n"
                                   "i=2 channels: Left, Right\r\n"
                                   "a=recvonly\r\n"
                                   "a=rtpmap:97 L24/48000/2\r\n"
                                   "a=ptime:0.125\r\n"
                                   "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0\r\n"
                                   "a=mediaclk:direct=1266592257\r\n";

// lib_aes67's sdp_to_string(): LF line endings handled too, ptime as %f, a
// nonzero PTP domain.
constexpr std::string_view LIB_AES67 = "v=0\n"
                                       "o=- 42 42 IN IP4 10.0.0.5\n"
                                       "s=XEBRA 1-8\n"
                                       "c=IN IP4 239.1.2.3\n"
                                       "t=0 0\n"
                                       "a=keywds:XMOS\n"
                                       "m=audio 5004 RTP/AVP 96\n"
                                       "i=XEBRA\n"
                                       "a=sendonly\n"
                                       "a=rtpmap:96 L24/96000/8\n"
                                       "a=ptime:1.000000\n"
                                       "a=ts-refclk:ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:3\n"
                                       "a=mediaclk:direct=0\n";

}  // namespace

TEST(sdp, parses_a_dante_announcement)
{
    auto const session = parse(DANTE);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->session_id, uint64_t{1311738121});
    EXPECT_EQ(session->origin, (ip::IPv4Address{192, 168, 1, 20}));
    EXPECT_EQ(session->name, std::string{"AVIO-Bluetooth : 2"});
    EXPECT_EQ(session->media.size(), size_t{1});

    auto const& m = session->media[0];
    EXPECT_EQ(m.port, uint16_t{5004});
    EXPECT_EQ(m.payload_type, uint8_t{97});
    EXPECT_TRUE(m.encoding == Encoding::l24);
    EXPECT_EQ(m.sample_rate, uint32_t{48000});
    EXPECT_EQ(m.channels, uint16_t{2});
    EXPECT_EQ(m.packet_time_us, uint32_t{125});
    EXPECT_EQ(m.frames_per_packet(), uint32_t{6});
    EXPECT_EQ(m.frame_bytes(), uint32_t{6});
    EXPECT_EQ(m.information, std::string{"2 channels: Left, Right"});

    EXPECT_TRUE(session->effective_direction(m) == Direction::recvonly);
    auto const connection = session->effective_connection(m);
    EXPECT_TRUE(connection.has_value());
    EXPECT_EQ(connection->address, (ip::IPv4Address{239, 69, 161, 58}));
    EXPECT_EQ(connection->ttl, uint8_t{32});
    auto const clock = session->effective_clock(m);
    EXPECT_TRUE(clock.has_value());
    EXPECT_EQ(clock->version, std::string{"IEEE1588-2008"});
    EXPECT_EQ(*clock->grandmaster, *ieee::eui64_from_string("00-1D-C1-FF-FE-0E-10-C4"));
    EXPECT_EQ(clock->domain, uint8_t{0});
    EXPECT_EQ(session->effective_media_clock_offset(m), uint32_t{1266592257});
}

TEST(sdp, parses_lib_aes67_output)
{
    auto const session = parse(LIB_AES67);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.size(), size_t{1});
    auto const& m = session->media[0];
    EXPECT_EQ(m.sample_rate, uint32_t{96000});
    EXPECT_EQ(m.channels, uint16_t{8});
    EXPECT_EQ(m.packet_time_us, uint32_t{1000});
    EXPECT_EQ(m.frames_per_packet(), uint32_t{96});
    EXPECT_EQ(session->effective_clock(m)->domain, uint8_t{3});
    EXPECT_EQ(session->effective_connection(m)->ttl, uint8_t{0});
}

TEST(sdp, packet_times_round_to_whole_frames)
{
    auto with_ptime = [](std::string_view ptime) {
        std::string text{DANTE};
        text.replace(text.find("0.125"), 5, ptime);
        return parse(text);
    };
    EXPECT_EQ(with_ptime("0.12")->media[0].frames_per_packet(), uint32_t{6});    // 5.76 frames
    EXPECT_EQ(with_ptime("0.333")->media[0].frames_per_packet(), uint32_t{16});  // 15.98
    EXPECT_EQ(with_ptime("4")->media[0].packet_time_us, uint32_t{4000});
    EXPECT_EQ(with_ptime("0.2505")->media[0].packet_time_us, uint32_t{251});  // rounds the 4th digit
    EXPECT_FALSE(with_ptime("0").has_value());
    EXPECT_FALSE(with_ptime("1.x").has_value());
}

TEST(sdp, media_level_values_override_the_session)
{
    std::string text{DANTE};
    text += "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n";
    text.replace(text.find("m=audio 5004 RTP/AVP 97\r\n") + 25, 0, "c=IN IP4 239.69.0.1/16\r\n");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.size(), size_t{1});
    auto const& m = session->media[0];
    EXPECT_EQ(session->effective_connection(m)->address, (ip::IPv4Address{239, 69, 0, 1}));
    EXPECT_FALSE(session->effective_clock(m)->grandmaster.has_value());  // the later, traceable one
}

TEST(sdp, parses_a_2022_7_pair_and_skips_other_media)
{
    constexpr std::string_view text = "v=0\r\n"
                                      "o=- 7 7 IN IP4 10.0.0.9\r\n"
                                      "s=Redundant\r\n"
                                      "t=0 0\r\n"
                                      "a=group:DUP primary secondary\r\n"
                                      "m=audio 5004 RTP/AVP 98\r\n"
                                      "c=IN IP4 239.10.0.1/32\r\n"
                                      "a=source-filter: incl IN IP4 239.10.0.1 10.0.0.9\r\n"
                                      "a=rtpmap:98 L16/48000/2\r\n"
                                      "a=mid:primary\r\n"
                                      "m=video 5006 RTP/AVP 96\r\n"
                                      "c=IN IP4 239.99.0.1/32\r\n"
                                      "a=rtpmap:96 raw/90000\r\n"
                                      "m=audio 5004 RTP/AVP 98\r\n"
                                      "c=IN IP4 239.20.0.1/32\r\n"
                                      "a=rtpmap:98 L16/48000/2\r\n"
                                      "a=mid:secondary\r\n";
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.size(), size_t{2});
    EXPECT_TRUE(session->media[0].encoding == Encoding::l16);
    EXPECT_EQ(session->media[0].packet_time_us, uint32_t{1000});  // AES67 default
    EXPECT_EQ(session->media[0].source_filter->source, (ip::IPv4Address{10, 0, 0, 9}));
    EXPECT_EQ(session->media[1].connection->address, (ip::IPv4Address{239, 20, 0, 1}));
    EXPECT_EQ(session->effective_media_clock_offset(session->media[1]), uint32_t{0});
}

TEST(sdp, refuses_what_the_bridge_cannot_use)
{
    auto changed = [](std::string_view from, std::string_view to) {
        std::string text{DANTE};
        text.replace(text.find(from), from.size(), to);
        return parse(text);
    };
    EXPECT_TRUE(parse("o=- 1 1 IN IP4 1.2.3.4\r\n").error() == SdpError::missing_field);  // no v=
    EXPECT_TRUE(changed("v=0", "v=1").error() == SdpError::unsupported_version);
    EXPECT_TRUE(changed("IN IP4 192", "IN IP6 192").error() == SdpError::unsupported_network);
    EXPECT_TRUE(changed("L24/", "L32/").error() == SdpError::unsupported_encoding);
    EXPECT_TRUE(changed("a=rtpmap:97", "a=rtpmap:98").error() == SdpError::missing_field);  // no rtpmap for 97
    EXPECT_TRUE(changed("c=IN IP4 239.69.161.58/32\r\n", "").error() == SdpError::missing_field);
    EXPECT_TRUE(changed("239.69.161.58", "239.69.161").error() == SdpError::malformed_line);
    EXPECT_TRUE(changed("/32", "/320").error() == SdpError::malformed_line);
    EXPECT_TRUE(changed("00-1D-C1", "zz-1D-C1").error() == SdpError::malformed_line);
    EXPECT_TRUE(changed("t=0 0", "t 0 0").error() == SdpError::malformed_line);
}

TEST(sdp, writes_what_it_parses)
{
    auto const session = parse(DANTE);
    auto const text = to_string(*session);
    EXPECT_EQ(
        text,
        std::string{"v=0\r\n"
                    "o=- 1311738121 1311738121 IN IP4 192.168.1.20\r\n"
                    "s=AVIO-Bluetooth : 2\r\n"
                    "c=IN IP4 239.69.161.58/32\r\n"
                    "t=0 0\r\n"
                    "m=audio 5004 RTP/AVP 97\r\n"
                    "i=2 channels: Left, Right\r\n"
                    "a=recvonly\r\n"
                    "a=rtpmap:97 L24/48000/2\r\n"
                    "a=ptime:0.125\r\n"
                    "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4\r\n"
                    "a=mediaclk:direct=1266592257\r\n"});
    auto const again = parse(text);
    EXPECT_TRUE(again.has_value());
    EXPECT_TRUE(*again == *session);
}

TEST(sdp, writes_a_sender_description)
{
    Session session;
    session.session_id = 99;
    session.session_version = 100;
    session.origin = ip::IPv4Address{10, 1, 2, 3};
    session.name = "Bridge out 1";
    session.clock = PtpReference{.grandmaster = ieee::eui64_from_string("00-11-22-ff-fe-33-44-55"), .domain = 2};
    session.media.push_back(Media{
        .port = 5004,
        .payload_type = 96,
        .channels = 8,
        .packet_time_us = 250,
        .direction = Direction::sendonly,
        .connection = Connection{.address = ip::IPv4Address{239, 1, 1, 1}, .ttl = 16},
        .media_clock_offset = 0});
    EXPECT_EQ(
        to_string(session),
        std::string{"v=0\r\n"
                    "o=- 99 100 IN IP4 10.1.2.3\r\n"
                    "s=Bridge out 1\r\n"
                    "t=0 0\r\n"
                    "a=ts-refclk:ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:2\r\n"
                    "m=audio 5004 RTP/AVP 96\r\n"
                    "c=IN IP4 239.1.1.1/16\r\n"
                    "a=sendonly\r\n"
                    "a=rtpmap:96 L24/48000/8\r\n"
                    "a=ptime:0.25\r\n"
                    "a=mediaclk:direct=0\r\n"});
}

TEST(sdp, keeps_the_ptp_clock_among_others)
{
    std::string text{DANTE};
    text += "a=ts-refclk:localmac=00-1D-C1-0E-10-C4\r\n";
    text += "a=mediaclk:sender\r\n";
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const& m = session->media.at(0);
    EXPECT_TRUE(session->effective_clock(m)->grandmaster.has_value());
    EXPECT_EQ(session->effective_media_clock_offset(m), uint32_t{1266592257});
}

TEST(sdp, reads_every_rfc_7273_form)
{
    auto clock_of = [](std::string_view refclk, std::string_view mediaclk) {
        std::string text{DANTE};
        text.replace(text.find("ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0"), 43, refclk);
        text.replace(text.find("direct=1266592257"), 17, mediaclk);
        return parse(text);
    };
    auto const nmbr = clock_of("ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:domain-nmbr=5", "direct");
    EXPECT_TRUE(nmbr.has_value());
    EXPECT_EQ(nmbr->media.at(0).clock->domain, uint8_t{5});
    EXPECT_EQ(nmbr->effective_media_clock_offset(nmbr->media.at(0)), uint32_t{0});

    auto const version_only = clock_of("ptp=IEEE802.1AS-2011", "direct=7 rate=48000/1");
    EXPECT_TRUE(version_only.has_value());
    EXPECT_EQ(version_only->media.at(0).clock->version, std::string{"IEEE802.1AS-2011"});
    EXPECT_FALSE(version_only->media.at(0).clock->grandmaster.has_value());
    EXPECT_EQ(version_only->effective_media_clock_offset(version_only->media.at(0)), uint32_t{7});

    auto const named = clock_of("ptp=IEEE1588-2002:00-1D-C1-FF-FE-0E-10-C4:domain-name=_DFLT", "direct=1");
    EXPECT_TRUE(named.has_value());
    EXPECT_EQ(named->media.at(0).clock->domain, uint8_t{0});

    EXPECT_FALSE(clock_of("ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:128", "direct").has_value());
}

TEST(sdp, reads_source_filters_at_either_level)
{
    std::string text{DANTE};
    text.replace(text.find("t=0 0\r\n") + 7, 0, "a=source-filter: incl IN IP4 * 192.168.1.20\r\n");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const filter = session->effective_source_filter(session->media.at(0));
    EXPECT_TRUE(filter.has_value());
    EXPECT_FALSE(filter->destination.has_value());
    EXPECT_EQ(filter->source, (ip::IPv4Address{192, 168, 1, 20}));
    auto const again = parse(to_string(*session));
    EXPECT_TRUE(again.has_value());
    EXPECT_TRUE(*again == *session);
}

TEST(sdp, encodings_are_case_insensitive_and_others_are_skipped)
{
    std::string text{DANTE};
    text.replace(text.find("L24/"), 3, "l24");
    text += "m=audio 5006 RTP/AVP 98\r\n"
            "a=rtpmap:98 AM824/48000/8\r\n";
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.size(), size_t{1});
    EXPECT_TRUE(session->media.at(0).encoding == Encoding::l24);
}

TEST(sdp, requires_a_name_and_timing)
{
    auto without = [](std::string_view line) {
        std::string text{DANTE};
        text.replace(text.find(line), line.size(), "");
        return parse(text);
    };
    EXPECT_TRUE(without("s=AVIO-Bluetooth : 2\r\n").error() == SdpError::missing_field);
    EXPECT_TRUE(without("t=0 0\r\n").error() == SdpError::missing_field);
}

TEST(sdp, writes_only_what_parses)
{
    auto session = parse(LIB_AES67);
    EXPECT_TRUE(session.has_value());
    session->username = "my host";
    session->name = "Out 1\r\na=source-filter: excl IN IP4 * 1.2.3.4";
    session->media.at(0).clock = PtpReference{.traceable = true};
    auto const text = to_string(*session);
    EXPECT_TRUE(text.find("o=- 42") != std::string::npos);
    EXPECT_TRUE(text.find("c=IN IP4 239.1.2.3/32\r\n") != std::string::npos);  // multicast needs a TTL
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n") != std::string::npos);
    auto const again = parse(text);
    EXPECT_TRUE(again.has_value());
    EXPECT_EQ(again->name, std::string{"Out 1  a=source-filter: excl IN IP4 * 1.2.3.4"});
    EXPECT_FALSE(again->source_filter.has_value());
}

TEST_MAIN(statusbar_sdp, sdp_test)
