// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/sdp/sdp.hpp"

#include "statusbar/test/test.hpp"

#include <optional>
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
    EXPECT_EQ(session->origin_address, std::string{"192.168.1.20"});
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
    auto const clocks = session->effective_clocks(m);
    EXPECT_EQ(clocks.size(), size_t{1});
    EXPECT_TRUE(clocks.front().version == PtpVersion::ieee1588_2008);
    EXPECT_EQ(*clocks.front().grandmaster, *ieee::eui64_from_string("00-1D-C1-FF-FE-0E-10-C4"));
    EXPECT_EQ(clocks.front().domain, uint8_t{0});
    EXPECT_EQ(session->effective_media_clock_offset(m).value_or(0), uint32_t{1266592257});
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
    EXPECT_EQ(session->effective_clocks(m).front().domain, uint8_t{3});
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
    auto const clocks = session->effective_clocks(m);  // both, in order (RFC 7273 Section 4.8)
    EXPECT_TRUE(clocks.size() == 2 && clocks[0].grandmaster.has_value() && clocks[1].traceable);
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
    // no a=mediaclk anywhere: the stream's timestamps are not known to be PTP-locked
    EXPECT_FALSE(session->effective_media_clock_offset(session->media[1]).has_value());
}

TEST(sdp, refuses_what_a_receiver_cannot_use)
{
    auto changed = [](std::string_view from, std::string_view to) {
        std::string text{DANTE};
        text.replace(text.find(from), from.size(), to);
        return parse(text);
    };
    EXPECT_TRUE(parse("o=- 1 1 IN IP4 1.2.3.4\r\n").error() == SdpError::missing_field);  // no v=
    EXPECT_TRUE(changed("v=0", "v=1").error() == SdpError::unsupported_version);
    EXPECT_TRUE(changed("IN IP4 192", "IN IP6 192").error() == SdpError::unsupported_network);
    EXPECT_TRUE(changed("L24/", "L20/").error() == SdpError::unsupported_encoding);
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
                    "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0\r\n"
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
    session.origin_address = "10.1.2.3";
    session.name = "Bridge out 1";
    session.clocks.ptp.push_back(PtpReference{.grandmaster = ieee::eui64_from_string("00-11-22-ff-fe-33-44-55"), .domain = 2});
    session.media.push_back(Media{
        .port = 5004,
        .payload_type = 96,
        .channels = 8,
        .packet_time_us = 250,
        .direction = Direction::sendonly,
        .connection = Connection{.address = ip::IPv4Address{239, 1, 1, 1}, .ttl = 16},
        .media_clock = MediaClock{.direct_offset = 0}});
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
    EXPECT_TRUE(session->effective_clocks(m).size() == 1 && session->effective_clocks(m).front().grandmaster.has_value());
    EXPECT_TRUE(m.clocks.other.size() == 1 && m.clocks.other.front() == "localmac=00-1D-C1-0E-10-C4");
    EXPECT_EQ(session->effective_media_clock_offset(m).value_or(0), uint32_t{1266592257});
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
    EXPECT_EQ(nmbr->media.at(0).clocks.ptp.front().domain, uint8_t{5});
    EXPECT_EQ(nmbr->effective_media_clock_offset(nmbr->media.at(0)).value_or(1), uint32_t{0});

    auto const version_only = clock_of("ptp=IEEE802.1AS-2011", "direct=7 rate=48000/1");
    EXPECT_TRUE(version_only.has_value());
    EXPECT_TRUE(version_only->media.at(0).clocks.ptp.front().version == PtpVersion::ieee802_1as);
    EXPECT_FALSE(version_only->media.at(0).clocks.ptp.front().grandmaster.has_value());
    EXPECT_EQ(version_only->effective_media_clock_offset(version_only->media.at(0)).value_or(0), uint32_t{7});

    auto const named = clock_of("ptp=IEEE1588-2002:00-1D-C1-FF-FE-0E-10-C4:domain-name=_DFLT", "direct=1");
    EXPECT_TRUE(named.has_value());
    EXPECT_EQ(named->media.at(0).clocks.ptp.front().domain, uint8_t{0});
    EXPECT_TRUE(named->media.at(0).clocks.ptp.front().version == PtpVersion::ieee1588_2002);

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
            "a=rtpmap:98 L20/48000/8\r\n";  // RFC 3190 L20: not carried
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.size(), size_t{1});
    EXPECT_TRUE(session->media.at(0).encoding == Encoding::l24);
}

TEST(sdp, carries_ravenna_aes3_as_am824)
{
    // RAVENNA's AES3 transport (and ST 2110-31): 32-bit IEC 61883-6 subframes
    std::string text{DANTE};
    text.replace(text.find("a=rtpmap:97 L24/48000/2"), 23, "a=rtpmap:97 AM824/48000/2");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.size(), size_t{1});
    EXPECT_TRUE(session->media.at(0).encoding == Encoding::am824);
    EXPECT_EQ(session->media.at(0).frame_bytes(), uint32_t{8});
    EXPECT_TRUE(to_string(*session).find("a=rtpmap:97 AM824/48000/2\r\n") != std::string::npos);
}

TEST(sdp, carries_32_bit_linear_pcm_as_l32)
{
    // RAVENNA's L32: 32-bit big-endian PCM, beside AES67's L16 and L24
    std::string text{DANTE};
    text.replace(text.find("a=rtpmap:97 L24/48000/2"), 23, "a=rtpmap:97 l32/48000/2");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.size(), size_t{1});
    EXPECT_TRUE(session->media.at(0).encoding == Encoding::l32);
    EXPECT_EQ(session->media.at(0).frame_bytes(), uint32_t{8});
    EXPECT_EQ(bytes_per_sample(Encoding::l16), uint32_t{2});
    EXPECT_EQ(bytes_per_sample(Encoding::l24), uint32_t{3});
    EXPECT_EQ(bytes_per_sample(Encoding::l32), uint32_t{4});
    auto const written = to_string(*session);
    EXPECT_TRUE(written.find("a=rtpmap:97 L32/48000/2\r\n") != std::string::npos);
    auto const again = parse(written);
    EXPECT_TRUE(again.has_value() && *again == *session);
}

TEST(sdp, keeps_every_offered_linear_pcm_format_in_m_line_order)
{
    std::string text{DANTE};
    text.replace(text.find("m=audio 5004 RTP/AVP 97"), 23, "m=audio 5004 RTP/AVP 98 97 99 96");
    text.replace(
        text.find("a=rtpmap:97 L24/48000/2"),
        23,
        "a=rtpmap:96 L16/48000/2\r\na=rtpmap:99 L20/48000/2\r\na=rtpmap:97 L24/48000/2\r\na=rtpmap:98 L32/48000/2");
    auto session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto& media = session->media.at(0);
    EXPECT_TRUE(
        (media.format() == PayloadFormat{.payload_type = 98, .encoding = Encoding::l32, .sample_rate = 48000, .channels = 2}));
    EXPECT_EQ(media.other_formats().size(), size_t{2});
    EXPECT_TRUE(
        media.other_formats().size() == 2 && media.other_formats()[0].encoding == Encoding::l24 &&
        media.other_formats()[1].encoding == Encoding::l16);

    // written back as offered, and read back the same
    auto const written = to_string(*session);
    EXPECT_TRUE(written.find("m=audio 5004 RTP/AVP 98 97 96\r\n") != std::string::npos);
    EXPECT_TRUE(
        written.find("a=rtpmap:98 L32/48000/2\r\na=rtpmap:97 L24/48000/2\r\na=rtpmap:96 L16/48000/2\r\n") != std::string::npos);
    auto const again = parse(written);
    EXPECT_TRUE(again.has_value() && *again == *session);

    // a receiver picks the one in its encoding; one not offered changes nothing
    EXPECT_TRUE(media.prefer(Encoding::l16));
    EXPECT_EQ(media.payload_type, uint8_t{96});
    EXPECT_TRUE(media.encoding == Encoding::l16);
    EXPECT_TRUE(media.other_formats().size() == 2 && media.other_formats()[1].encoding == Encoding::l32);
    EXPECT_FALSE(media.prefer(Encoding::am824));
    EXPECT_TRUE(media.encoding == Encoding::l16);
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
    session->media.at(0).clocks.ptp = {PtpReference{.traceable = true}};
    auto const text = to_string(*session);
    EXPECT_TRUE(text.find("o=- 42") != std::string::npos);
    EXPECT_TRUE(text.find("c=IN IP4 239.1.2.3/32\r\n") != std::string::npos);  // multicast needs a TTL
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n") != std::string::npos);
    auto const again = parse(text);
    EXPECT_TRUE(again.has_value());
    EXPECT_EQ(again->name, std::string{"Out 1  a=source-filter: excl IN IP4 * 1.2.3.4"});
    EXPECT_FALSE(again->source_filter.has_value());
}

TEST(sdp, a_media_clock_that_is_not_direct_overrides_a_direct_one)
{
    // RFC 7273 section 5: the media-level attribute is the one in force
    std::string text{DANTE};
    text.replace(text.find("a=mediaclk:direct=1266592257"), 28, "a=mediaclk:sender");
    text.replace(text.find("t=0 0\r\n") + 7, 0, "a=mediaclk:direct=5\r\n");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.size(), size_t{1});
    EXPECT_FALSE(session->effective_media_clock_offset(session->media.at(0)).has_value());
    EXPECT_TRUE(session->media_clock == std::optional{MediaClock{.direct_offset = 5}});
    // and it is written back as sender
    EXPECT_TRUE(to_string(*session).find("a=mediaclk:sender\r\n") != std::string::npos);
}

TEST(sdp, picks_the_first_offered_payload_type_it_can_carry)
{
    std::string text{DANTE};
    text.replace(text.find("m=audio 5004 RTP/AVP 97"), 23, "m=audio 5004 RTP/AVP 0 99 97");
    text.replace(text.find("a=rtpmap:97 L24/48000/2"), 0, "a=rtpmap:99 L20/48000/2\r\n");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.size(), size_t{1});
    EXPECT_EQ(session->media.at(0).payload_type, uint8_t{97});
    EXPECT_TRUE(session->media.at(0).encoding == Encoding::l24);

    // only a static payload type, with no rtpmap (RFC 3551 PCMU): skipped, not an error
    std::string pcmu{DANTE};
    pcmu.replace(pcmu.find("m=audio 5004 RTP/AVP 97"), 23, "m=audio 5004 RTP/AVP 0");
    pcmu.erase(pcmu.find("a=rtpmap:97 L24/48000/2\r\n"), 25);
    auto const skipped = parse(pcmu);
    EXPECT_FALSE(skipped.has_value());
    EXPECT_TRUE(skipped.error() == SdpError::unsupported_encoding);

    // a dynamic payload type with no rtpmap is a malformed description
    std::string dynamic{DANTE};
    dynamic.erase(dynamic.find("a=rtpmap:97 L24/48000/2\r\n"), 25);
    EXPECT_TRUE(parse(dynamic).error() == SdpError::missing_field);
}

TEST(sdp, accepts_a_host_name_origin_and_a_wildcard_source_filter)
{
    std::string text{DANTE};
    text.replace(text.find("IN IP4 192.168.1.20"), 19, "IN IP4 studio-node.local");
    text.replace(text.find("t=0 0\r\n") + 7, 0, "a=source-filter: incl IN * 239.69.161.58 192.168.1.20\r\n");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->origin_address, std::string{"studio-node.local"});
    EXPECT_TRUE(session->source_filter.has_value());
    EXPECT_EQ(session->source_filter->source, (ip::IPv4Address{192, 168, 1, 20}));
    EXPECT_TRUE(to_string(*session).find("o=- 1311738121 1311738121 IN IP4 studio-node.local\r\n") != std::string::npos);

    // a filter on another network is not ours to apply, and not an error
    std::string ip6{DANTE};
    ip6.replace(ip6.find("t=0 0\r\n") + 7, 0, "a=source-filter: incl IN IP6 FF1E::1 2001:db8::1\r\n");
    auto const ignored = parse(ip6);
    EXPECT_TRUE(ignored.has_value());
    EXPECT_FALSE(ignored->source_filter.has_value());
}

TEST(sdp, writes_only_what_the_rfcs_allow)
{
    Session session;
    session.origin_address = "not a token";
    session.clocks.ptp = {PtpReference{}, PtpReference{.version = PtpVersion::other, .traceable = true}};  // no valid form
    session.media.push_back(Media{
        .port = 5004,
        .payload_type = 96,
        .connection = Connection{.address = ip::IPv4Address{10, 1, 2, 3}, .ttl = 16},  // unicast: no TTL
    });
    auto const text = to_string(session);
    EXPECT_TRUE(text.find("o=- 0 0 IN IP4 0.0.0.0\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("ts-refclk") == std::string::npos);
    EXPECT_TRUE(text.find("c=IN IP4 10.1.2.3\r\n") != std::string::npos);
    EXPECT_TRUE(parse(text).has_value());
}

TEST(sdp, writes_domain_zero_and_reads_both_forms)
{
    std::string text{DANTE};
    text.replace(text.find(":0\r\n"), 2, "");  // the bare form, no domain
    auto const bare = parse(text);
    EXPECT_TRUE(bare.has_value());
    EXPECT_EQ(bare->media.at(0).clocks.ptp.front().domain, uint8_t{0});
    EXPECT_TRUE(to_string(*bare).find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0\r\n") != std::string::npos);
}

TEST(sdp, carries_the_ravenna_clock_domain)
{
    std::string text{DANTE};
    text += "a=clock-domain:PTPv2 3\r\n";
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const& m = session->media.at(0);
    EXPECT_EQ(m.clock_domain, std::optional<uint8_t>{3});
    EXPECT_EQ(session->effective_clock_domain(m), std::optional<uint8_t>{3});
    auto const written = to_string(*session);
    EXPECT_TRUE(written.find("a=mediaclk:direct=1266592257\r\na=clock-domain:PTPv2 3\r\n") != std::string::npos);
    EXPECT_TRUE(parse(written) == session);

    // Session level, read before the first m=, applies to every media.
    std::string level{DANTE};
    level.insert(level.find("m=audio"), "a=clock-domain:PTPv2 1\r\n");
    auto const at_session = parse(level);
    EXPECT_TRUE(at_session.has_value());
    if (!at_session) {
        return;
    }
    EXPECT_EQ(at_session->clock_domain, std::optional<uint8_t>{1});
    EXPECT_FALSE(at_session->media.at(0).clock_domain.has_value());
    EXPECT_EQ(at_session->effective_clock_domain(at_session->media.at(0)), std::optional<uint8_t>{1});

    // Other kinds and domains past 127 are ignored.
    std::string other{DANTE};
    other += "a=clock-domain:IEEE1588-2002 _DFLT\r\na=clock-domain:PTPv2 200\r\n";
    auto const ignored = parse(other);
    EXPECT_TRUE(ignored.has_value());
    EXPECT_FALSE(ignored.transform([](Session const& s) { return s.media.at(0).clock_domain.has_value(); }).value_or(true));
}

TEST(sdp, reads_and_writes_the_rtcp_port)
{
    std::string text{DANTE};
    text += "a=rtcp:5007\r\n";
    auto session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_TRUE(session->media[0].rtcp == (RtcpAttribute{.port = 5007, .address = std::nullopt}));
    EXPECT_FALSE(session->media[0].rtcp_mux);

    session->media[0].rtcp = RtcpAttribute{.port = 6001, .address = ip::IPv4Address{239, 1, 2, 4}};
    auto const written = to_string(*session);
    EXPECT_TRUE(written.find("a=rtcp:6001 IN IP4 239.1.2.4\r\n") != std::string::npos);
    auto const again = parse(written);
    EXPECT_TRUE(again.has_value());
    EXPECT_TRUE(again->media[0].rtcp == session->media[0].rtcp);
}

TEST(sdp, a_malformed_rtcp_port_is_refused)
{
    for (std::string_view const line :
         {"a=rtcp:0\r\n", "a=rtcp:x\r\n", "a=rtcp:5005 IN IP6 ::1\r\n", "a=rtcp:5005 IN IP4 nope\r\n"}) {
        std::string text{DANTE};
        text += line;
        EXPECT_FALSE(parse(text).has_value());
    }
}

TEST(sdp, rtcp_mux_is_read_and_bandwidth_lines_are_ignored)
{
    std::string text{DANTE};
    text += "b=RS:0\r\nb=RR:2000\r\na=rtcp-mux\r\n";
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_TRUE(session->media[0].rtcp_mux);
    EXPECT_TRUE(to_string(*session).find("rtcp-mux") == std::string::npos);
}

TEST(sdp, mediaclk_sender_round_trips)
{
    // RFC 7273 section 5: an asynchronous sender keeps the grandmaster's ts-refclk
    std::string text{DANTE};
    text.replace(text.find("a=mediaclk:direct=1266592257"), 28, "a=mediaclk:sender");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const& m = session->media.at(0);
    EXPECT_TRUE(m.media_clock == std::optional{MediaClock::sender()});
    EXPECT_TRUE(session->effective_media_clock(m)->kind() == MediaClockKind::sender);
    EXPECT_FALSE(session->effective_media_clock_offset(m).has_value());
    EXPECT_TRUE(session->effective_clocks(m).front().grandmaster.has_value());
    std::string const written = to_string(*session);
    EXPECT_TRUE(written.find("a=mediaclk:sender\r\n") != std::string::npos);
    EXPECT_TRUE(written.find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0\r\n") != std::string::npos);
    auto const again = parse(written);
    EXPECT_TRUE(again.has_value());
    EXPECT_TRUE(again->media.at(0).media_clock == m.media_clock);

    // Another kind is neither direct nor sender.
    text.replace(text.find("a=mediaclk:sender"), 17, "a=mediaclk:IEEE1722=00-1B-21-FF-FE-00-00-01");
    auto const stream = parse(text);
    EXPECT_TRUE(stream->effective_media_clock(stream->media.at(0))->kind() == MediaClockKind::other);
    EXPECT_TRUE(session->effective_media_clock(session->media.at(0)) != stream->effective_media_clock(stream->media.at(0)));
    EXPECT_TRUE(MediaClock::direct(7).kind() == MediaClockKind::direct);
    EXPECT_FALSE(parse(LIB_AES67)->effective_media_clock(parse(LIB_AES67)->media.at(0)) == std::optional{MediaClock::sender()});
}

TEST(sdp, the_clock_values_are_the_attribute_text)
{
    PtpReference const clock{.grandmaster = *ieee::eui64_from_string("EC-46-70-FF-FE-10-FF-B0"), .domain = 127};
    EXPECT_EQ(ts_refclk_value(clock), std::string{"ptp=IEEE1588-2008:EC-46-70-FF-FE-10-FF-B0:127"});
    EXPECT_EQ(ts_refclk_value(PtpReference{}), std::string{});
    EXPECT_EQ(media_clock_value(MediaClock{.direct_offset = 0}), std::string{"direct=0"});
    EXPECT_EQ(media_clock_value(MediaClock{}), std::string{"sender"});
}

TEST(sdp_ts_refclk, every_reference_at_a_level_is_kept_in_order)
{
    std::string text{DANTE};
    text += "a=ts-refclk:ptp=IEEE802.1AS-2011:00-1D-C1-FF-FE-0E-10-C4\r\n";
    text += "a=ts-refclk:ptp=IEEE1588-2019:00-1D-C1-FF-FE-0E-10-C4:0\r\n";
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const clocks = session->effective_clocks(session->media.at(0));
    EXPECT_EQ(clocks.size(), size_t{3});
    EXPECT_TRUE(clocks[1].version == PtpVersion::ieee802_1as && clocks[2].version == PtpVersion::ieee1588_2019);
    auto const written = to_string(*session);
    EXPECT_TRUE(written.find("a=ts-refclk:ptp=IEEE802.1AS-2011:00-1D-C1-FF-FE-0E-10-C4\r\n") != std::string::npos);
    EXPECT_TRUE(written.find("a=ts-refclk:ptp=IEEE1588-2019:00-1D-C1-FF-FE-0E-10-C4:0\r\n") != std::string::npos);
    EXPECT_TRUE(parse(written) == session);
}

TEST(sdp_ts_refclk, references_beyond_the_capacity_give_way_to_ones_that_name_a_clock)
{
    std::string text{"v=0\r\no=- 1 1 IN IP4 10.0.0.1\r\ns=x\r\nc=IN IP4 239.69.1.1/32\r\nt=0 0\r\nm=audio 5004 RTP/AVP 97\r\n"
                     "a=rtpmap:97 L24/48000/2\r\n"};
    for (size_t i = 0; i < ReferenceClocks::MAX; ++i) {
        text += "a=ts-refclk:ptp=IEEE1588-2030:junk\r\n";  // unknown: kept, naming nothing
    }
    text += "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0\r\n";
    text += "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n";
    text += "a=ts-refclk:ptp=IEEE1588-2031:more\r\n";  // full, and names nothing: dropped
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const& clocks = session->media.at(0).clocks.ptp;
    EXPECT_EQ(clocks.size(), ReferenceClocks::MAX);
    EXPECT_TRUE(clocks[0].grandmaster.has_value() && clocks[1].traceable && clocks[2].version == PtpVersion::other);
}

TEST(sdp_ts_refclk, an_unknown_version_never_fails_the_session_and_is_written_as_read)
{
    std::string text{DANTE};
    text += "a=ts-refclk:ptp=IEEE1588-2030:not-a-gmid:999\r\n";
    text += "a=ts-refclk:ptp=IEEE1588-2008\r\n";  // a bare version
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const written = to_string(*session);
    EXPECT_TRUE(written.find("a=ts-refclk:ptp=IEEE1588-2030:not-a-gmid:999\r\n") != std::string::npos);
    EXPECT_TRUE(written.find("a=ts-refclk:ptp=IEEE1588-2008\r\n") != std::string::npos);
    EXPECT_EQ(ts_refclk_value(session->media.at(0).clocks.ptp[1]), std::string{"ptp=IEEE1588-2030:not-a-gmid:999"});
}

TEST(sdp_ts_refclk, every_reference_round_trips_with_a_media_level_override)
{
    std::string text{DANTE};
    text.erase(text.find("a=ts-refclk:"), std::string_view{"a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0\r\n"}.size());
    text.replace(text.find("t=0 0\r\n") + 7, 0, "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0\r\n");
    text += "a=ts-refclk:localmac=00-1D-C1-0E-10-C4\r\na=ts-refclk:ptp=IEEE802.1AS-2030:x\r\n";
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const again = parse(to_string(*session));
    EXPECT_TRUE(again == session);
    EXPECT_TRUE(again && again->effective_clocks(again->media.at(0)).size() == 1);  // the media level's unknown one
    EXPECT_TRUE(again && !again->names_clock(again->media.at(0), session->clocks.ptp.front()));
}

TEST(sdp_ts_refclk, a_media_level_reference_overrides_the_sessions)
{
    std::string text{DANTE};
    auto const clock_line = text.find("a=ts-refclk:");
    text.erase(clock_line, text.find("\r\n", clock_line) + 2 - clock_line);
    text.replace(text.find("t=0 0\r\n") + 7, 0, "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0\r\n");
    auto session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->effective_clocks(session->media.at(0)).size(), size_t{1});  // the session's
    text += "a=ts-refclk:localmac=00-1D-C1-0E-10-C4\r\n";
    session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_TRUE(session->effective_clocks(session->media.at(0)).empty());  // the media's: no PTP clock
    EXPECT_TRUE(session->clocks.other.empty());
}

TEST(sdp_ts_refclk, an_802_1as_reference_is_written_without_a_domain)
{
    auto const gm = ieee::eui64_from_string("39-A7-94-FF-FE-07-CB-D0");
    PtpReference const gptp{.version = PtpVersion::ieee802_1as, .grandmaster = gm};
    EXPECT_EQ(
        ts_refclk_value(gptp), std::string{"ptp=IEEE802.1AS-2011:39-A7-94-FF-FE-07-CB-D0"});  // AES67-2023 Clause 8.2, Example 3
    EXPECT_EQ(
        ts_refclk_value(PtpReference{.version = PtpVersion::ieee802_1as, .traceable = true}),
        std::string{"ptp=IEEE802.1AS-2011:traceable"});
    EXPECT_EQ(ts_refclk_value(PtpReference{.version = PtpVersion::other, .grandmaster = gm}), std::string{});
}

TEST(sdp_ts_refclk, each_version_token_reads)
{
    auto version_of = [](std::string_view const token) {
        std::string text{DANTE};
        text.replace(text.find("IEEE1588-2008"), 13, token);
        auto const session = parse(text);
        return session ? std::optional{session->media.at(0).clocks.ptp.front()} : std::nullopt;
    };
    EXPECT_TRUE(version_of("IEEE1588-2002")->version == PtpVersion::ieee1588_2002);
    EXPECT_TRUE(version_of("IEEE1588-2019")->version == PtpVersion::ieee1588_2019);
    EXPECT_TRUE(version_of("IEEE802.1AS-2011")->version == PtpVersion::ieee802_1as);
    EXPECT_TRUE(version_of("IEEE802.1AS-2020")->version == PtpVersion::ieee802_1as);  // the unregistered spelling
    EXPECT_TRUE(version_of("IEEE1588-2030")->version == PtpVersion::other);
    EXPECT_FALSE(version_of("IEEE1588-2030")->grandmaster.has_value());  // kept unparsed, as read
    EXPECT_EQ(version_of("IEEE1588-2030")->text, std::string{"ptp=IEEE1588-2030:00-1D-C1-FF-FE-0E-10-C4:0"});
}

TEST(sdp_ts_refclk, an_802_1as_reference_written_with_a_domain_reads_as_domain_zero)
{
    std::string text{DANTE};
    text.replace(text.find("IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0"), 39, "IEEE802.1AS-2011:00-1D-C1-FF-FE-0E-10-C4:5");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.at(0).clocks.ptp.front().domain, uint8_t{0});
    EXPECT_TRUE(to_string(*session).find("ptp=IEEE802.1AS-2011:00-1D-C1-FF-FE-0E-10-C4\r\n") != std::string::npos);
}

TEST(sdp_ts_refclk, the_same_grandmaster_in_the_same_domain_is_the_same_clock)
{
    auto const gm = ieee::eui64_from_string("00-1D-C1-FF-FE-10-6C-16");
    auto const other = ieee::eui64_from_string("00-1D-C1-FF-FE-0E-10-C4");
    PtpReference const ours{.grandmaster = gm};
    EXPECT_TRUE(ours.same_clock(PtpReference{.grandmaster = gm}));
    EXPECT_FALSE(ours.same_clock(PtpReference{.grandmaster = other}));
    EXPECT_FALSE(ours.same_clock(PtpReference{.grandmaster = gm, .domain = 1}));  // AES67-2023 Clause 8.2: another domain
    EXPECT_FALSE(ours.same_clock(PtpReference{.traceable = true}));
    EXPECT_FALSE(ours.same_clock(PtpReference{}));
}

TEST(sdp_ts_refclk, ieee_802_1as_and_ieee_1588_in_domain_zero_are_one_clock)
{
    auto const gm = ieee::eui64_from_string("00-1D-C1-FF-FE-10-6C-16");
    PtpReference const gptp{.version = PtpVersion::ieee802_1as, .grandmaster = gm};
    EXPECT_TRUE(gptp.same_clock(PtpReference{.grandmaster = gm}));
    PtpReference const v2019{.version = PtpVersion::ieee1588_2019, .grandmaster = gm};
    EXPECT_TRUE(v2019.same_clock(gptp));
    EXPECT_FALSE(gptp.same_clock(PtpReference{.grandmaster = gm, .domain = 3}));
    EXPECT_FALSE(gptp.same_clock(PtpReference{.version = PtpVersion::ieee1588_2002, .grandmaster = gm}));  // PTPv1
    EXPECT_FALSE(gptp.same_clock(PtpReference{.version = PtpVersion::other, .grandmaster = gm}));
}

TEST(sdp_ts_refclk, a_session_names_a_clock_when_any_reference_in_force_does)
{
    std::string text{DANTE};
    text += "a=ts-refclk:ptp=IEEE802.1AS-2011:00-1D-C1-FF-FE-10-6C-16\r\n";
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const& m = session->media.at(0);
    EXPECT_TRUE(session->names_clock(m, PtpReference{.grandmaster = ieee::eui64_from_string("00-1D-C1-FF-FE-10-6C-16")}));
    EXPECT_TRUE(session->names_clock(m, PtpReference{.grandmaster = ieee::eui64_from_string("00-1D-C1-FF-FE-0E-10-C4")}));
    EXPECT_FALSE(session->names_clock(m, PtpReference{.grandmaster = ieee::eui64_from_string("00-11-22-FF-FE-33-44-55")}));
}

TEST(sdp_ts_refclk, a_session_without_a_reference_names_no_clock)
{
    std::string text{DANTE};
    text.erase(text.find("a=ts-refclk:"), std::string_view{"a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0E-10-C4:0\r\n"}.size());
    auto const session = parse(text);  // RFC 7273 Section 6: a local clock is assumed
    EXPECT_TRUE(session.has_value() && session->effective_clocks(session->media.at(0)).empty());
    EXPECT_FALSE(session->names_clock(
        session->media.at(0), PtpReference{.grandmaster = ieee::eui64_from_string("00-1D-C1-FF-FE-0E-10-C4")}));
}

//
// ST 2110-30 Section 6.2.2 channel order, RFC 8866 Section 6.5 maxptime
//

namespace {

// Shaped like an ST 2110-30 sender's: level C, 125 us, the 2059-2 default domain, channel order and maxptime.
constexpr std::string_view ST2110 = "v=0\r\n"
                                    "o=- 1443716955 1443716955 IN IP4 192.168.1.30\r\n"
                                    "s=2110 sender\r\n"
                                    "t=0 0\r\n"
                                    "m=audio 5004 RTP/AVP 97 98\r\n"
                                    "c=IN IP4 232.10.1.1/64\r\n"
                                    "a=source-filter: incl IN IP4 232.10.1.1 192.168.1.30\r\n"
                                    "a=rtpmap:97 L24/48000/8\r\n"
                                    "a=rtpmap:98 L16/48000/8\r\n"
                                    "a=fmtp:98 channel-order=SMPTE2110.(ST,ST,ST,ST)\r\n"
                                    "a=fmtp:97 channel-order=SMPTE2110.(51,ST)\r\n"
                                    "a=ptime:0.125\r\n"
                                    "a=maxptime:0.125\r\n"
                                    "a=ts-refclk:ptp=IEEE1588-2008:08-00-11-FF-FE-21-E1-B0:127\r\n"
                                    "a=mediaclk:direct=0\r\n";

/// ST2110 with its primary fmtp line replaced by `fmtp` (empty: removed).
auto with_fmtp(std::string_view const fmtp) -> std::string
{
    std::string text{ST2110};
    std::string const line = "a=fmtp:97 channel-order=SMPTE2110.(51,ST)\r\n";
    text.replace(text.find(line), line.size(), fmtp);
    return text;
}

}  // namespace

TEST(sdp_channel_order, the_primary_formats_order_is_read)
{
    auto const session = parse(ST2110);
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(session->media.at(0).channel_order, std::string{"SMPTE2110.(51,ST)"});
    EXPECT_EQ(session->media.at(0).max_packet_time_us, std::optional<uint32_t>{125});
}

TEST(sdp_channel_order, a_preferred_alternative_does_not_adopt_the_primarys_order)
{
    auto const session = parse(ST2110);
    EXPECT_EQ(session->media.at(0).other_formats().size(), size_t{1});
    EXPECT_EQ(session->media.at(0).other_formats().front().payload_type, uint8_t{98});
}

TEST(sdp_channel_order, an_order_is_written_and_read_back)
{
    auto session = parse(ST2110);
    auto const text = to_string(*session);
    EXPECT_TRUE(text.find("a=fmtp:97 channel-order=SMPTE2110.(51,ST)\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=ptime:0.125\r\na=maxptime:0.125\r\n") != std::string::npos);
    auto const again = parse(text);
    EXPECT_TRUE(again.has_value());
    EXPECT_EQ(again->media.at(0).channel_order, session->media.at(0).channel_order);
    EXPECT_EQ(again->media.at(0).max_packet_time_us, session->media.at(0).max_packet_time_us);
}

TEST(sdp_channel_order, a_malformed_order_is_neither_kept_nor_written)
{
    for (auto const* const fmtp :
         {"a=fmtp:97 channel-order=SMPTE2110.(XY)\r\n",
          "a=fmtp:97 channel-order=SMPTE2110.(ST,)\r\n",
          "a=fmtp:97 channel-order=SMPTE2110.()\r\n",
          "a=fmtp:97 channel-order=OTHER.(ST)\r\n",
          "a=fmtp:x channel-order=SMPTE2110.(ST)\r\n",
          "a=fmtp:99 channel-order=SMPTE2110.(ST)\r\n"}) {
        auto const session = parse(with_fmtp(fmtp));
        EXPECT_TRUE(session.has_value());
        EXPECT_TRUE(session->media.at(0).channel_order.empty());
    }
    Session session = *parse(ST2110);
    session.media.at(0).channel_order = "SMPTE2110.(ST);x=1";
    EXPECT_TRUE(to_string(session).find("a=fmtp:") == std::string::npos);
}

TEST(sdp_channel_order, other_parameters_beside_it_are_passed_over)
{
    auto const session = parse(with_fmtp("a=fmtp:97 foo=1; Channel-Order=SMPTE2110.(M,U03)\r\n"));
    EXPECT_EQ(session->media.at(0).channel_order, std::string{"SMPTE2110.(M,U03)"});
}

TEST(sdp_channel_order, an_absent_order_and_maxptime_are_not_written)
{
    auto session = parse(with_fmtp(""));
    EXPECT_TRUE(session->media.at(0).channel_order.empty());
    session->media.at(0).max_packet_time_us.reset();
    auto const text = to_string(*session);
    EXPECT_TRUE(text.find("a=fmtp:") == std::string::npos);
    EXPECT_TRUE(text.find("a=maxptime:") == std::string::npos);
}

TEST(sdp_channel_order, a_malformed_maxptime_is_ignored)
{
    std::string text{ST2110};
    text.replace(text.find("a=maxptime:0.125"), 16, "a=maxptime:x");
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    EXPECT_FALSE(session->media.at(0).max_packet_time_us.has_value());
}

TEST(sdp_channel_order, each_group_counts_its_channels)
{
    EXPECT_EQ(channel_order_channels("SMPTE2110.(M)"), std::optional<uint32_t>{1});
    EXPECT_EQ(channel_order_channels("SMPTE2110.(DM,ST,LtRt)"), std::optional<uint32_t>{6});
    EXPECT_EQ(channel_order_channels("SMPTE2110.(51,71,222,SGRP)"), std::optional<uint32_t>{42});
    EXPECT_EQ(channel_order_channels("SMPTE2110.(U64,U64,U08)"), std::optional<uint32_t>{136});
    EXPECT_EQ(channel_order_channels("SMPTE2110.(U00)"), std::nullopt);
    EXPECT_EQ(channel_order_channels("SMPTE2110.(U65)"), std::nullopt);
    EXPECT_EQ(channel_order_channels("SMPTE2110.(U1)"), std::nullopt);
    EXPECT_EQ(channel_order_channels("SMPTE2110.(st)"), std::nullopt);
    EXPECT_EQ(channel_order_channels("SMPTE2110.ST"), std::nullopt);
    EXPECT_EQ(channel_order_channels(""), std::nullopt);
}

TEST(sdp_ipmx, the_declaration_is_read_beside_the_channel_order)
{
    auto const session = parse(with_fmtp("a=fmtp:97 channel-order=SMPTE2110.(51,ST); IPMX; measuredsamplerate=47952\r\n"));
    EXPECT_TRUE(session.has_value());
    EXPECT_TRUE(session->media.at(0).ipmx);
    EXPECT_EQ(session->media.at(0).channel_order, std::string{"SMPTE2110.(51,ST)"});
    EXPECT_TRUE(parse(with_fmtp("a=fmtp:97 ipmx\r\n"))->media.at(0).ipmx);
    EXPECT_FALSE(parse(ST2110)->media.at(0).ipmx);
}

TEST(sdp_ipmx, each_fmtp_line_of_a_payload_type_counts)
{
    auto const session = parse(with_fmtp("a=fmtp:97 IPMX\r\na=fmtp:97 channel-order=SMPTE2110.(51,ST)\r\n"));
    EXPECT_TRUE(session.has_value());
    EXPECT_TRUE(session && session->media.at(0).ipmx);
    EXPECT_TRUE(session && session->media.at(0).channel_order == "SMPTE2110.(51,ST)");
}

TEST(sdp_ipmx, another_payload_types_declaration_is_not_the_primarys)
{
    auto const session = parse(with_fmtp("a=fmtp:99 IPMX\r\n"));
    EXPECT_FALSE(session->media.at(0).ipmx);
    auto const other = parse(with_fmtp("a=fmtp:98 IPMX\r\n"));  // 98's first fmtp line is the channel order's
    EXPECT_FALSE(other->media.at(0).ipmx);
}

TEST(sdp_ipmx, the_declaration_is_written_after_the_channel_order_or_alone)
{
    auto session = *parse(ST2110);
    session.media.at(0).ipmx = true;
    auto text = to_string(session);
    EXPECT_TRUE(text.find("a=fmtp:97 channel-order=SMPTE2110.(51,ST); IPMX\r\n") != std::string::npos);
    EXPECT_TRUE(parse(text)->media.at(0).ipmx);
    session.media.at(0).channel_order.clear();
    text = to_string(session);
    EXPECT_TRUE(text.find("a=fmtp:97 IPMX\r\n") != std::string::npos);
    EXPECT_TRUE(parse(text)->media.at(0).ipmx);
}

// RFC 7273 Section 4.8: a reference with no domain is domain 0's, so it never names a domain-127 grandmaster (ST 2110-10
// senders write the domain)
TEST(sdp_ts_refclk, a_reference_without_a_domain_names_domain_zero_alone)
{
    std::string text{ST2110};
    text.erase(text.find(":127\r\n"), 4);
    auto const session = parse(text);
    EXPECT_TRUE(session.has_value());
    auto const& media = session->media.at(0);
    auto const grandmaster = *ieee::eui64_from_string("08-00-11-FF-FE-21-E1-B0");
    EXPECT_FALSE(session->names_clock(media, PtpReference::naming(grandmaster, 127)));
    EXPECT_TRUE(session->names_clock(media, PtpReference::naming(grandmaster, 0)));
}

TEST(sdp, at_most_max_media_audio_sections_are_read)
{
    std::string text = "v=0\r\no=- 1 1 IN IP4 10.0.0.1\r\ns=x\r\nc=IN IP4 239.69.1.1/32\r\nt=0 0\r\n";
    for (size_t i = 0; i < Session::MAX_MEDIA; ++i) {
        text += "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L24/48000/2\r\n";
    }
    auto const full = parse(text);
    EXPECT_EQ(full.transform([](Session const& s) { return s.media.size(); }).value_or(0), Session::MAX_MEDIA);
    text += "m=video 5006 RTP/AVP 98\r\n";  // skipped: not counted
    EXPECT_TRUE(parse(text).has_value());
    text += "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L24/48000/2\r\n";
    EXPECT_TRUE(parse(text).error() == SdpError::too_many_media);
}

TEST(sdp, long_text_fields_are_written_whole)
{
    // a field of a multiple of 256 characters (llvm-project issue 154670), as a SAP announcement may carry
    for (size_t const length : {255UZ, 256UZ, 512UZ, 1024UZ}) {
        std::string const long_text(length, 'x');
        std::string text{DANTE};
        text.replace(text.find("o=-"), 3, "o=" + long_text);
        text.replace(text.find("192.168.1.20"), 12, long_text);
        text.replace(text.find("AVIO-Bluetooth : 2"), 18, long_text);
        text.replace(text.find("2 channels: Left, Right"), 23, long_text);
        text.replace(text.find("t=0 0\r\n") + 7, 0, "a=ts-refclk:" + long_text + "\r\n");
        auto const session = parse(text);
        EXPECT_TRUE(session.has_value());
        if (!session) {
            continue;
        }
        auto const again = parse(to_string(*session));
        EXPECT_TRUE(again.has_value() && *again == *session);
        EXPECT_EQ(ts_refclk_text(session->clocks), long_text);
    }
}

TEST(sdp, a_channel_order_of_256_characters_is_written_whole)
{
    std::string order{"SMPTE2110.(ST,ST,ST"};
    for (int i = 0; i < 59; ++i) {
        order += ",U01";
    }
    order += ")";
    EXPECT_EQ(order.size(), size_t{256});
    std::string text{DANTE};
    text.replace(text.find("a=ptime"), 0, "a=fmtp:97 channel-order=" + order + "\r\n");
    auto const session = parse(text);
    bool const one_media = session.has_value() && session->media.size() == 1;
    EXPECT_TRUE(one_media);
    if (!one_media) {
        return;
    }
    EXPECT_EQ(session->media.front().channel_order, order);
    EXPECT_TRUE(to_string(*session).find("a=fmtp:97 channel-order=" + order + "\r\n") != std::string::npos);
}

TEST_MAIN(statusbar_sdp, sdp_test)
