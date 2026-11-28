// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/aes67/aes67_sender.hpp"

#include "padl/aes67/aes67_error.hpp"
#include "padl/aes67/aes67_receiver.hpp"
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

namespace {

/// One stream sent twice from the same pattern: as packets, and as frames written to a frame port whose engine sends
/// its run by a PacketSchedule (LoopTransport::FrameTx::send_due(), after each tick).
struct TwoSenders
{
    explicit TwoSenders(uint32_t const start_lead = 0, uint32_t const phase = 0)
    {
        frames.frame_ports = true;
        frames.start_lead = start_lead;
        packet.set_packet_phase(phase);
        frame.set_packet_phase(phase);
    }
    /// Both configured to `destination`.
    auto configure(Destination const& destination) -> bool
    {
        return packet.configure(destination).has_value() && frame.configure(destination).has_value();
    }
    /// Both ticked at `now_index`, then the frame port's engine.
    void tick(uint64_t const now_index)
    {
        packet.tick(now_index, 0);
        frame.tick(now_index, 0);
        if (frames.frame_tx != nullptr) {
            frames.frame_tx->send_due(now_index);
        }
    }
    /// Both sent the same packets (sequence numbers by their difference from the first), the frame path's run
    /// starting `skip` packets after the packet path's.
    void expect_same_packets(size_t const skip = 0) const
    {
        EXPECT_EQ(packets.queue.size(), frames.queue.size() + skip);
        auto const a0 = padl::rtp::parse_rtp(packets.queue.at(skip).bytes);
        auto const b0 = padl::rtp::parse_rtp(frames.queue.at(0).bytes);
        for (size_t i = 0; i < std::min(packets.queue.size() - skip, frames.queue.size()); ++i) {
            auto const a = padl::rtp::parse_rtp(packets.queue[skip + i].bytes);
            auto const b = padl::rtp::parse_rtp(frames.queue[i].bytes);
            EXPECT_TRUE(a && b && a0 && b0 && a->header.timestamp.get() == b->header.timestamp.get());
            EXPECT_TRUE(
                a && b && std::ranges::equal(a->payload, b->payload) && b->header.payload_type() == a->header.payload_type());
            EXPECT_TRUE(
                a && b && a0 && b0 &&
                static_cast<uint16_t>(a->header.sequence.get() - a0->header.sequence.get()) ==
                    static_cast<uint16_t>(b->header.sequence.get() - b0->header.sequence.get()));
            EXPECT_TRUE(b && b->header.ssrc.get() == frame.ssrc());
        }
    }

    LoopTransport packets;
    LoopTransport frames;
    PatternSource packet_source{6};
    PatternSource frame_source{6};
    Sender packet{STEREO_L24, packet_source.provider(), packets, "Test"};
    Sender frame{STEREO_L24, frame_source.provider(), frames, "Test"};
};

Destination const GROUP{.address = IPv4Address{239, 69, 1, 2}, .port = 5004, .payload_type = 98};

}  // namespace

TEST(aes67_sender, announces_l32_that_an_l32_receiver_accepts)
{
    LoopTransport transport;
    PatternSource source{8};
    Sender sender{STEREO_L32, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004, .payload_type = 98}));
    sender.set_grandmaster(GRANDMASTER);
    std::string const text = padl::sdp::to_string(*sender.session());
    EXPECT_TRUE(text.find("m=audio 5004 RTP/AVP 98\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=rtpmap:98 L32/48000/2\r\n") != std::string::npos);
    auto const parsed = padl::sdp::parse(text);
    EXPECT_TRUE(parsed.has_value());
    RecordingSink sink;
    Receiver receiver{STEREO_L32, sink.handler(), transport};
    EXPECT_TRUE(receiver.configure(*parsed));
    EXPECT_EQ(STEREO_L32.bytes_per_frame(), uint32_t{8});
}

// A packet is handed to the transport departing at the time of the tick that sends it, whatever
// timescale the caller ticks in (the sending NIC's PHC: PTP time, or a member interface's domain
// time); a tick without a time sends untimed.
TEST(aes67_sender, a_packet_departs_at_the_time_of_the_tick_that_sends_it)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};  // 48 frames a packet at 48 kHz
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004, .payload_type = 98}));
    EXPECT_EQ(sender.send_offset(), uint32_t{6});
    constexpr uint64_t T0 = 1'700'000'000'000'000'000ULL;
    sender.tick(54, T0 + 1'125'000);  // the tick that sends packet 0 (last frame 47, send offset 6)
    sender.tick(60, T0 + 1'250'000);  // nothing due
    sender.tick(102, T0 + 2'125'000);
    EXPECT_TRUE((transport.departures == std::vector<uint64_t>{T0 + 1'125'000, T0 + 2'125'000}));
    sender.tick(150, 0);  // untimed
    EXPECT_TRUE(!transport.departures.empty() && transport.departures.back() == 0);
    // A stalled sender catches up in one tick: every packet of the burst departs at that tick.
    sender.tick(300, T0 + 6'250'000);
    EXPECT_TRUE(transport.departures.size() >= 5 && transport.departures.back() == T0 + 6'250'000);
}

TEST(aes67_sender, writes_sdp_that_parses_back)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);  // these tests time packets by their last frame
    EXPECT_FALSE(sender.session().has_value());
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004, .payload_type = 98}));
    sender.set_grandmaster(GRANDMASTER);

    auto const session = sender.session();
    EXPECT_TRUE(session.has_value());
    std::string const text = padl::sdp::to_string(*session);
    auto const parsed = padl::sdp::parse(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->origin_address, std::string{"192.168.1.10"});
    EXPECT_EQ(parsed->name, std::string{"Test"});
    EXPECT_EQ(parsed->media.size(), size_t{1});
    auto const& media = parsed->media[0];
    EXPECT_EQ(media.port, uint16_t{5004});
    EXPECT_EQ(media.payload_type, uint8_t{98});
    EXPECT_TRUE(media.encoding == padl::sdp::Encoding::l24);
    EXPECT_EQ(media.channels, uint16_t{2});
    EXPECT_EQ(media.sample_rate, uint32_t{48000});
    EXPECT_EQ(media.packet_time_us, uint32_t{1000});
    EXPECT_EQ(media.frames_per_packet(), FPP);
    EXPECT_TRUE(parsed->effective_direction(media) == padl::sdp::Direction::recvonly);
    EXPECT_TRUE(parsed->effective_media_clock_offset(media) == uint32_t{0});
    auto const clocks = parsed->effective_clocks(media);
    EXPECT_EQ(clocks.size(), size_t{1});
    EXPECT_TRUE(clocks.front().grandmaster == ieee::eui64_from_string("00-1D-C1-FF-FE-12-34-56"));
    auto const connection = parsed->effective_connection(media);
    EXPECT_TRUE(connection.has_value());
    EXPECT_TRUE(connection->address == (IPv4Address{239, 69, 1, 2}));
    EXPECT_EQ(connection->ttl, uint8_t{32});
    auto const filter = parsed->effective_source_filter(media);
    EXPECT_TRUE(filter.has_value());
    EXPECT_TRUE(filter->source == (IPv4Address{192, 168, 1, 10}));
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=mediaclk:direct=0\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=clock-domain:PTPv2 0\r\n") != std::string::npos);
    EXPECT_TRUE(parsed->effective_clock_domain(media) == uint8_t{0});

    // Reconfiguring bumps the version and keeps the session id.
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 3}, .port = 5004}));
    auto const again = sender.session();
    EXPECT_EQ(again->session_id, session->session_id);
    EXPECT_EQ(again->session_version, session->session_version + 1);

    // A grandmaster in another domain names it, one version on; the same
    // grandmaster again changes nothing; none leaves ts-refclk out.
    sender.set_grandmaster(Grandmaster{.identity = GRANDMASTER.identity, .domain = 5});
    EXPECT_TRUE(
        padl::sdp::to_string(*sender.session()).find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:5\r\n") !=
        std::string::npos);
    EXPECT_EQ(sender.session()->session_version, again->session_version + 1);
    EXPECT_EQ(sender.session()->session_id, session->session_id);
    sender.set_grandmaster(Grandmaster{.identity = GRANDMASTER.identity, .domain = 5});
    EXPECT_EQ(sender.session()->session_version, again->session_version + 1);
    sender.set_grandmaster(std::nullopt);
    EXPECT_EQ(sender.session()->session_version, again->session_version + 2);
    EXPECT_TRUE(padl::sdp::to_string(*sender.session()).find("ts-refclk") == std::string::npos);
    EXPECT_TRUE(padl::sdp::to_string(*sender.session()).find("clock-domain") == std::string::npos);

    // A new name is a new s=, one version on, the same session id; the same name changes nothing.
    sender.set_name("Bridge : Test");
    EXPECT_EQ(sender.name(), std::string{"Bridge : Test"});
    EXPECT_EQ(sender.session()->name, std::string{"Bridge : Test"});
    EXPECT_EQ(sender.session()->session_version, again->session_version + 3);
    EXPECT_EQ(sender.session()->session_id, session->session_id);
    sender.set_name("Bridge : Test");
    EXPECT_EQ(sender.session()->session_version, again->session_version + 3);
    // A replacement sender continues the session under its name.
    Sender replacement{STEREO_L24, source.provider(), transport, "Test"};
    replacement.continue_from(sender);
    EXPECT_EQ(replacement.name(), std::string{"Bridge : Test"});
}

TEST(aes67_sender, an_802_1as_grandmaster_is_named_without_a_domain)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    sender.set_grandmaster(
        Grandmaster{.identity = GRANDMASTER.identity, .domain = 0, .version = padl::sdp::PtpVersion::ieee802_1as});
    std::string const text = padl::sdp::to_string(*sender.session());
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE802.1AS-2011:00-1D-C1-FF-FE-12-34-56\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("clock-domain") == std::string::npos);  // RAVENNA's PTPv2 domain: none for gPTP
    auto const parsed = padl::sdp::parse(text);
    EXPECT_TRUE(parsed && parsed->names_clock(parsed->media.at(0), GRANDMASTER.reference()));  // domain 0, one clock
}

TEST(aes67_sender, a_traceable_grandmaster_is_named_and_said_traceable)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    sender.set_grandmaster(GRANDMASTER);
    auto const version = sender.session()->session_version;
    auto const info_block = sender.info_block_version();
    EXPECT_TRUE(padl::sdp::to_string(*sender.session()).find("traceable") == std::string::npos);
    sender.set_grandmaster(Grandmaster{.identity = GRANDMASTER.identity, .domain = 0, .traceable = true});
    EXPECT_EQ(sender.session()->session_version, version + 1);  // a change of synchronisation status (Clause 8.2)
    EXPECT_EQ(sender.info_block_version(), info_block);         // its ts-refclk names the grandmaster alone
    std::string const text = padl::sdp::to_string(*sender.session());
    auto const named = text.find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0\r\n");
    auto const traceable = text.find("a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n");
    EXPECT_TRUE(named != std::string::npos && traceable != std::string::npos && named < traceable);
    auto const parsed = padl::sdp::parse(text);
    EXPECT_TRUE(parsed.has_value());
    auto const clocks = parsed->effective_clocks(parsed->media.at(0));
    EXPECT_EQ(clocks.size(), size_t{2});
    EXPECT_TRUE(clocks.size() == 2 && clocks[0].grandmaster == GRANDMASTER.identity && clocks[1].traceable);
}

TEST(aes67_sender, a_traceable_802_1as_grandmaster_is_said_traceable_under_its_version)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    sender.set_grandmaster(Grandmaster{
        .identity = GRANDMASTER.identity, .domain = 0, .version = padl::sdp::PtpVersion::ieee802_1as, .traceable = true});
    std::string const text = padl::sdp::to_string(*sender.session());
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE802.1AS-2011:00-1D-C1-FF-FE-12-34-56\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE802.1AS-2011:traceable\r\n") != std::string::npos);
}

TEST(aes67_sender, writes_a_125_us_ptime)
{
    LoopTransport transport;
    PatternSource source{6};
    auto format = STEREO_L24;
    format.frames_per_packet = 6;
    Sender sender{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    std::string const text = padl::sdp::to_string(*sender.session());
    EXPECT_TRUE(text.find("a=ptime:0.125\r\n") != std::string::npos);
    EXPECT_EQ(padl::sdp::parse(text)->media[0].frames_per_packet(), uint32_t{6});
}

TEST(aes67_sender, refuses_what_aes67_cannot_carry)
{
    LoopTransport transport;
    PatternSource source{6};
    auto format = STEREO_L24;
    format.sample_rate = 44100;
    Sender rate{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(rate.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}).error() == Aes67Error::unsupported_format);

    format = STEREO_L24;
    format.channels = 16;  // 48 frames of 16 x L24 is 2304 bytes
    Sender big{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(big.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}).error() == Aes67Error::packet_too_large);
}

TEST(aes67_sender, payloads_are_at_most_1440_octets)
{
    LoopTransport transport;
    PatternSource source{6};
    // AES67-2023 Clause 6.3: 60 x L24 x 8 frames is 1440 octets, 2 x L16 x 361 is 1444.
    StreamFormat format{.encoding = padl::sdp::Encoding::l24, .channels = 60, .sample_rate = 48000, .frames_per_packet = 8};
    Sender fits{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(fits.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}).has_value());
    format = {.encoding = padl::sdp::Encoding::l16, .channels = 2, .sample_rate = 48000, .frames_per_packet = 361};
    Sender over{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(over.configure(Destination{.address = IPv4Address{239, 69, 1, 3}}).error() == Aes67Error::packet_too_large);
}

TEST(aes67_sender, sends_packets_on_the_grid_when_due)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);  // these tests time packets by their last frame
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));

    uint64_t const base = 48'000ULL * 1'000'000;  // some PTP time well past the epoch
    sender.tick(base + 46, 0);                    // packet [base - 48, base) is due: last index base - 1
    EXPECT_EQ(transport.queue.size(), size_t{1});
    sender.tick(base + 46, 0);  // nothing new
    EXPECT_EQ(transport.queue.size(), size_t{1});
    sender.tick(base + 47, 0);  // [base, base + 48) completes
    EXPECT_EQ(transport.queue.size(), size_t{2});
    sender.tick(base + 47 + (3 * 48), 0);  // three more at once
    EXPECT_EQ(transport.queue.size(), size_t{5});

    uint16_t expected_seq = 0;
    for (size_t i = 0; i < transport.queue.size(); ++i) {
        auto const packet = padl::rtp::parse_rtp(transport.queue[i].bytes);
        EXPECT_TRUE(packet.has_value());
        uint64_t const first = base - 48 + (i * 48);
        EXPECT_EQ(packet->header.timestamp.get(), static_cast<uint32_t>(first));
        EXPECT_EQ(packet->header.ssrc.get(), sender.ssrc());
        EXPECT_EQ(packet->header.payload_type(), uint8_t{96});
        EXPECT_FALSE(packet->header.marker());
        if (i > 0) {
            EXPECT_EQ(packet->header.sequence.get(), expected_seq);
        }
        expected_seq = static_cast<uint16_t>(packet->header.sequence.get() + 1);
        EXPECT_EQ(packet->payload.size(), size_t{FPP} * 6);
        EXPECT_EQ(packet->payload[0], pattern(first, 0));
        EXPECT_EQ(packet->payload[(47 * 6) + 5], pattern(first + 47, 5));
    }
    EXPECT_EQ(sender.counters().packets, uint64_t{5});
    EXPECT_EQ(sender.counters().frames, uint64_t{5 * 48});
    EXPECT_TRUE(sender.state() == State::running);

    // Reset: the counters start again, the run (its SSRC and sequence) goes on.
    uint32_t const ssrc = sender.ssrc();
    sender.reset_counters();
    EXPECT_EQ(sender.counters().packets, uint64_t{0});
    sender.tick(base + 47 + (4 * 48), 0);
    EXPECT_EQ(sender.counters().packets, uint64_t{1});
    EXPECT_EQ(transport.queue.size(), size_t{6});
    auto const next = padl::rtp::parse_rtp(transport.queue.back().bytes);
    EXPECT_TRUE(next && next->header.ssrc.get() == ssrc && next->header.sequence.get() == expected_seq);
    EXPECT_TRUE(sender.state() == State::running);
}

TEST(aes67_sender, sends_silence_for_frames_not_held_and_skips_when_far_behind)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);  // these tests time packets by their last frame
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));

    uint64_t const base = 4800 * 48;
    sender.tick(base - 1, 0);  // sends [base - 48, base)
    source.refuse_index = base;
    sender.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    auto const silent = padl::rtp::parse_rtp(transport.queue[1].bytes);
    EXPECT_TRUE(std::ranges::all_of(silent->payload, [](uint8_t b) { return b == 0; }));
    EXPECT_EQ(sender.counters().lost, uint64_t{48});

    // 100 packets later: only the newest is sent, the rest counted lost.
    transport.queue.clear();
    sender.tick(base + 47 + (100 * 48), 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_EQ(padl::rtp::parse_rtp(transport.queue[0].bytes)->header.timestamp.get(), static_cast<uint32_t>(base + (100 * 48)));
    EXPECT_EQ(sender.counters().lost, uint64_t{48 + (99 * 48)});
}

TEST(aes67_sender, keeps_its_session_id_across_restarts)
{
    // Two senders for the same stream, as before and after a restart: the
    // same o= session id, and a version at least the wall clock's seconds.
    auto const start = [](std::string name, IPv4Address const& group) {
        auto transport = std::make_unique<LoopTransport>();
        auto sender = std::make_unique<Sender>(STEREO_L24, FrameProvider{}, *transport, std::move(name));
        EXPECT_TRUE(sender->configure(Destination{.address = group, .port = 5004}));
        auto session = *sender->session();
        return session;
    };
    auto const first = start("Test", IPv4Address{239, 69, 1, 2});
    auto const again = start("Test", IPv4Address{239, 69, 1, 2});
    EXPECT_EQ(again.session_id, first.session_id);
    EXPECT_NE(first.session_id, uint64_t{0});
    EXPECT_EQ(first.session_id >> 63U, uint64_t{0});  // positive in a signed reading
    EXPECT_TRUE(first.session_version > uint64_t{1'700'000'000});
    EXPECT_TRUE(again.session_version >= first.session_version);
    EXPECT_NE(start("Other", IPv4Address{239, 69, 1, 2}).session_id, first.session_id);
    EXPECT_EQ(start("Test", IPv4Address{239, 69, 1, 3}).session_id, first.session_id);  // moved, not a new stream

    // One the caller gives is kept (to 63 bits), across reconfigures too.
    LoopTransport transport;
    Sender sender{STEREO_L24, FrameProvider{}, transport, "Test"};
    sender.set_session_id(0xFFFF'0000'0000'1234ULL);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    EXPECT_EQ(sender.session()->session_id, uint64_t{0x7FFF'0000'0000'1234ULL});
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 4}}));
    EXPECT_EQ(sender.session()->session_id, uint64_t{0x7FFF'0000'0000'1234ULL});
    // A grandmaster known before the destination: the version still starts at the clock.
    Sender early{STEREO_L24, FrameProvider{}, transport, "Test"};
    early.set_grandmaster(GRANDMASTER);
    EXPECT_TRUE(early.configure(Destination{.address = IPv4Address{239, 69, 1, 5}}));
    EXPECT_TRUE(early.session()->session_version > uint64_t{1'700'000'000});
    EXPECT_EQ(Sender::stable_session_id("aes67-out-0"), Sender::stable_session_id("aes67-out-0"));
    EXPECT_NE(Sender::stable_session_id("aes67-out-0"), Sender::stable_session_id("aes67-out-1"));
}

TEST(aes67_sender, waits_out_the_send_offset)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_EQ(sender.send_offset(), uint32_t{6});  // the default: one 125 us tick
    EXPECT_EQ(Sender::default_send_offset(StreamFormat{.encoding = padl::sdp::Encoding::l24, .sample_rate = 44100}), uint32_t{6});
    EXPECT_EQ(Sender::default_send_offset(StreamFormat{.encoding = padl::sdp::Encoding::l24, .sample_rate = 96000}), uint32_t{12});
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    sender.set_send_offset(100);

    uint64_t const base = 48'000ULL * 1000;  // a multiple of 48
    sender.tick(base + 47 + 99, 0);          // [base, base + 48) is one frame short of due
    size_t const before = transport.queue.size();
    sender.tick(base + 47 + 100, 0);  // due now
    EXPECT_EQ(transport.queue.size(), before + 1);
    EXPECT_EQ(padl::rtp::parse_rtp(transport.queue.back().bytes)->header.timestamp.get(), static_cast<uint32_t>(base));
}

TEST(aes67_sender, sends_uncounted_silence_with_no_provider)
{
    LoopTransport transport;
    Sender sender{STEREO_L24, FrameProvider{}, transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    sender.tick(48'000ULL * 1000 + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_TRUE(std::ranges::all_of(padl::rtp::parse_rtp(transport.queue[0].bytes)->payload, [](uint8_t b) { return b == 0; }));
    EXPECT_EQ(sender.counters().lost, uint64_t{0});
    EXPECT_EQ(sender.counters().packets, uint64_t{1});
}

TEST(aes67_sender, a_paused_sender_sends_nothing_and_resumes_on_the_grid_as_a_new_source)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    uint64_t const base = 48'000ULL * 1'000'000;
    sender.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_TRUE(sender.state() == State::running);

    sender.pause();
    EXPECT_TRUE(sender.state() == State::idle);
    EXPECT_EQ(sender.send_state().ssrc, uint32_t{0});
    auto const last = padl::rtp::parse_rtp(transport.queue.back().bytes);

    // Ten packets later: a new run from the newest due packet under a new SSRC (RFC 3550 Section 8).
    sender.tick(base + 47 + (10 * uint64_t{FPP}), 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    auto const resumed = padl::rtp::parse_rtp(transport.queue.back().bytes);
    EXPECT_TRUE(last && resumed);
    EXPECT_EQ(resumed->header.timestamp.get(), static_cast<uint32_t>(base + (10 * uint64_t{FPP})));
    EXPECT_NE(resumed->header.ssrc.get(), last->header.ssrc.get());
    EXPECT_NE(resumed->header.ssrc.get(), uint32_t{0});
    EXPECT_EQ(resumed->header.ssrc.get(), sender.ssrc());
    EXPECT_TRUE(sender.state() == State::running);
    EXPECT_EQ(sender.send_state().ssrc, sender.ssrc());
    EXPECT_EQ(sender.counters().lost, uint64_t{0});

    Sender unconfigured{STEREO_L24, source.provider(), transport, "None"};
    unconfigured.pause();
    EXPECT_TRUE(unconfigured.state() == State::idle);
}

TEST(aes67_sender, a_paused_sender_whose_transport_fails_reports_error)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    uint64_t const base = 48'000ULL * 1'000'000;
    sender.tick(base + 47, 0);
    transport.failing = std::error_code{EINVAL, std::system_category()};
    sender.tick(base + 95, 0);
    EXPECT_TRUE(sender.state() == State::error);
    sender.pause();
    EXPECT_TRUE(sender.state() == State::error);  // paused, but the transport still cannot send
    EXPECT_TRUE(sender.error() == std::errc::invalid_argument);
    sender.pause();
    EXPECT_TRUE(sender.state() == State::error);
    transport.failing.clear();
    sender.pause();
    EXPECT_TRUE(sender.state() == State::idle);
    EXPECT_FALSE(sender.error());
}

TEST(aes67_sender, am824_silence_is_the_mbla_word_in_every_sample)
{
    constexpr StreamFormat AM824{
        .encoding = padl::sdp::Encoding::am824, .channels = 3, .sample_rate = 48000, .frames_per_packet = FPP};
    uint64_t const base = 48'000ULL * 1000;

    // No provider: uncounted silence, every channel's sample the MBLA word.
    LoopTransport transport;
    Sender idle{AM824, FrameProvider{}, transport, "Test"};
    idle.set_send_offset(0);
    EXPECT_TRUE(idle.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    idle.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    auto const silent = padl::rtp::parse_rtp(transport.queue[0].bytes);
    EXPECT_EQ(silent->payload.size(), size_t{AM824.bytes_per_packet()});
    EXPECT_TRUE(all_words_are(silent->payload, 0x4000'0000));
    EXPECT_EQ(idle.counters().lost, uint64_t{0});

    // A provider that wrote part of the packet and reports it missing: all silence, counted lost.
    transport.queue.clear();
    FrameProvider partial = [](uint64_t, uint32_t, std::span<uint8_t> const out) {
        std::ranges::fill(out.first(out.size() / 2), uint8_t{0x55});
        return Supply::missing;
    };
    Sender sender{AM824, std::move(partial), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 3}}));
    sender.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_TRUE(all_words_are(padl::rtp::parse_rtp(transport.queue[0].bytes)->payload, 0x4000'0000));
    EXPECT_EQ(sender.counters().lost, uint64_t{FPP});

    // The same provider on L24: zero bytes, as before.
    transport.queue.clear();
    FrameProvider partial_l24 = [](uint64_t, uint32_t, std::span<uint8_t> const out) {
        std::ranges::fill(out.first(out.size() / 2), uint8_t{0x55});
        return Supply::missing;
    };
    Sender l24{STEREO_L24, std::move(partial_l24), transport, "Test"};
    l24.set_send_offset(0);
    EXPECT_TRUE(l24.configure(Destination{.address = IPv4Address{239, 69, 1, 4}}));
    l24.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_TRUE(std::ranges::all_of(padl::rtp::parse_rtp(transport.queue[0].bytes)->payload, [](uint8_t b) { return b == 0; }));
}

TEST(aes67_sender, waits_out_a_small_backward_step)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    uint64_t const base = 48'000ULL * 1000;
    for (uint64_t k = 0; k < 4; ++k) {
        sender.tick(base + 47 + (k * FPP), 0);  // sends [base, ...) .. [base + 3 * 48, ...)
    }
    EXPECT_EQ(transport.queue.size(), size_t{4});
    // Two packets back: nothing is sent again, and sending resumes where it left off.
    sender.tick(base + 47 + FPP, 0);
    sender.tick(base + 47 + (3 * FPP), 0);
    EXPECT_EQ(transport.queue.size(), size_t{4});
    sender.tick(base + 47 + (4 * FPP), 0);
    EXPECT_EQ(transport.queue.size(), size_t{5});
    EXPECT_EQ(padl::rtp::parse_rtp(transport.queue.back().bytes)->header.timestamp.get(), static_cast<uint32_t>(base + (4 * FPP)));
    for (size_t i = 1; i < transport.queue.size(); ++i) {
        auto const previous = padl::rtp::parse_rtp(transport.queue[i - 1].bytes)->header.timestamp.get();
        EXPECT_EQ(padl::rtp::parse_rtp(transport.queue[i].bytes)->header.timestamp.get(), previous + FPP);  // none repeated
    }
}

TEST(aes67_sender, resynchronises_after_a_large_backward_step)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    uint64_t const base = 48'000ULL * 1000;
    sender.tick(base + 47, 0);
    // A step back of more than MAX_BURST packets: start again from now.
    uint64_t const back = base - (uint64_t{Sender::MAX_BURST + 2} * FPP);
    sender.tick(back + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    EXPECT_EQ(padl::rtp::parse_rtp(transport.queue.back().bytes)->header.timestamp.get(), static_cast<uint32_t>(back));
}

TEST(aes67_sender, a_run_has_sent_once_a_packet_has_gone)
{
    LoopTransport transport;
    transport.refuse = true;  // no TX buffer: nothing goes
    Sender sender{STEREO_L24, {}, transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    uint64_t const base = uint64_t{4800} * 48;
    sender.tick(base - 1, 0);
    EXPECT_TRUE(sender.send_state().next_index != sender.send_state().first_index);  // tried
    EXPECT_FALSE(sender.send_state().sent);
    transport.refuse = false;
    sender.tick(base + 47, 0);
    EXPECT_TRUE(sender.send_state().sent);
    sender.pause();
    EXPECT_FALSE(sender.send_state().sent);
}

TEST(aes67_sender, still_pulls_and_counts_a_packet_the_transport_cannot_accept)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));

    uint64_t const base = 4800 * 48;
    sender.tick(base - 1, 0);  // [base - 48, base), rendered in place
    EXPECT_EQ(source.pulls, size_t{1});
    // No TX buffer: the frames are still pulled (into the staging buffer), the
    // missing packet still counted lost, and both packets counted dropped.
    transport.refuse = true;
    source.refuse_index = base + 48;
    sender.tick(base + 95, 0);
    EXPECT_EQ(source.pulls, size_t{3});
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_EQ(sender.counters().dropped, uint64_t{2});
    EXPECT_EQ(sender.counters().lost, uint64_t{48});
    EXPECT_EQ(sender.counters().packets, uint64_t{1});
}

TEST(aes67_sender, a_packet_phase_moves_the_send_tick_not_the_timestamps)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    sender.set_packet_phase(18);
    EXPECT_EQ(sender.packet_phase(), uint32_t{18});
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));

    uint64_t const base = 48'000ULL * 1'000'000;  // a multiple of 48
    sender.tick(base + 16, 0);                    // starts with [base - 78, base - 30)
    EXPECT_EQ(transport.queue.size(), size_t{1});
    sender.tick(base + 16, 0);  // [base - 30, base + 18) completes at base + 17
    EXPECT_EQ(transport.queue.size(), size_t{1});
    sender.tick(base + 17, 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    sender.tick(base + 17 + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    sender.tick(base + 17 + 48, 0);  // [base + 18, base + 66)
    EXPECT_EQ(transport.queue.size(), size_t{3});
    for (size_t i = 0; i < transport.queue.size(); ++i) {
        auto const packet = padl::rtp::parse_rtp(transport.queue[i].bytes);
        uint64_t const first = base - 78 + (i * 48);
        // The timestamp is still the first frame's index: receivers place the samples exactly.
        EXPECT_EQ(packet->header.timestamp.get(), static_cast<uint32_t>(first));
        EXPECT_EQ(packet->payload[0], pattern(first, 0));
        EXPECT_EQ(packet->payload[(47 * 6) + 5], pattern(first + 47, 5));
    }
    // The phase is reduced modulo the packet.
    sender.set_packet_phase(48 + 6);
    EXPECT_EQ(sender.packet_phase(), uint32_t{6});
}

TEST(aes67_sender, stagger_phases_spread_eight_senders_over_eight_ticks)
{
    // 1 ms at 48 kHz: 0, 6, ... 42, then again.
    for (uint32_t k = 0; k < 16; ++k) {
        EXPECT_EQ(Sender::stagger_phase(STEREO_L24, k), (k % 8) * 6);
    }
    // 96 kHz: a tick is 12 frames.
    StreamFormat const hi{.encoding = padl::sdp::Encoding::l24, .channels = 2, .sample_rate = 96000, .frames_per_packet = 96};
    EXPECT_EQ(Sender::stagger_phase(hi, 3), uint32_t{36});
    // A packet of one tick: every sender sends every tick anyway.
    StreamFormat const short_packets{
        .encoding = padl::sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = 6};
    EXPECT_EQ(Sender::stagger_phase(short_packets, 5), uint32_t{0});
    // 250 us: two ticks.
    StreamFormat const quarter{.encoding = padl::sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = 12};
    EXPECT_EQ(Sender::stagger_phase(quarter, 1), uint32_t{6});
    EXPECT_EQ(Sender::stagger_phase(quarter, 2), uint32_t{0});
}

TEST(aes67_sender, reports_the_transports_send_error)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    uint64_t const base = 4800 * 48;
    sender.tick(base - 1, 0);
    EXPECT_TRUE(sender.state() == State::running);
    transport.failing = std::error_code{EINVAL, std::system_category()};
    sender.tick(base + 47, 0);
    EXPECT_TRUE(sender.state() == State::error);
    EXPECT_TRUE(sender.error() == std::errc::invalid_argument);
    sender.tick(base + 47, 0);  // nothing due: still in error
    EXPECT_TRUE(sender.state() == State::error);
    transport.failing.clear();
    sender.tick(base + 47, 0);  // nothing due, but the transport sends again
    EXPECT_TRUE(sender.state() == State::ready);
    EXPECT_FALSE(sender.error());
    sender.tick(base + 95, 0);
    EXPECT_TRUE(sender.state() == State::running);
}

TEST(aes67_sender, a_sender_on_a_drifting_index_sends_every_packet_once)
{
    // A domain 100 ppm fast or slow of PTP: ticks on the 125 us PTP grid see its index advance by
    // 5, 6 or 7 frames, and a 125 us stream sends 0, 1 or 2 packets a tick.
    auto const clock = *padl::mediaclock::PtpMediaClock::make(48000);
    uint64_t const t0 = 1'790'000'000ULL * 1'000'000'000ULL;
    for (uint32_t const fpp : {6U, 48U}) {
        for (double const rate_offset : {100e-6, -100e-6}) {
            LoopTransport transport;
            PatternSource source{6};
            auto format = STEREO_L24;
            format.frames_per_packet = fpp;
            Sender sender{format, source.provider(), transport, "Drift"};
            EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
            padl::mediaclock::Timebase const timebase{
                .reference_ns = static_cast<int64_t>(t0),
                .domain_ns = static_cast<int64_t>(t0),
                .rate_offset = rate_offset,
                .running = true};
            std::array<size_t, 9> advances{};
            uint64_t previous = 0;
            for (uint64_t tick = 0; tick < 24'000; ++tick) {  // 3 s
                auto const t = static_cast<int64_t>(t0 + (tick * 125'000));
                uint64_t const index = clock.index_at(static_cast<uint64_t>(timebase.to_domain(t)));
                if (tick > 0) {
                    ++advances[std::min<uint64_t>(index - previous, advances.size() - 1)];
                }
                previous = index;
                sender.tick(index, 0);
            }
            EXPECT_EQ(advances[6] + advances[rate_offset > 0 ? 7 : 5], size_t{23'999});
            EXPECT_TRUE(advances[rate_offset > 0 ? 7 : 5] > 0);

            // Every packet once, in order: timestamps one packet apart, sequence numbers one apart.
            EXPECT_TRUE(transport.queue.size() > 1);
            auto const first = padl::rtp::parse_rtp(transport.queue.front().bytes);
            uint32_t const first_ts = first->header.timestamp.get();
            uint16_t const first_seq = first->header.sequence.get();
            bool in_order = true;
            for (size_t i = 0; i < transport.queue.size(); ++i) {
                auto const packet = padl::rtp::parse_rtp(transport.queue[i].bytes);
                uint32_t const ts = packet->header.timestamp.get();
                in_order = in_order && ts == first_ts + static_cast<uint32_t>(i * fpp) &&
                    packet->header.sequence.get() == static_cast<uint16_t>(first_seq + i) && packet->payload[0] == pattern(ts, 0);
            }
            EXPECT_TRUE(in_order);
            EXPECT_EQ(sender.counters().packets, uint64_t{transport.queue.size()});
            EXPECT_EQ(sender.counters().lost, uint64_t{0});
            EXPECT_EQ(sender.counters().dropped, uint64_t{0});
        }
    }
}

TEST(aes67_sender, a_foreign_domain_output_announces_mediaclk_sender)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Foreign"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    sender.set_grandmaster(GRANDMASTER);
    auto const before = *sender.session();
    uint8_t const info_version = sender.info_block_version();
    EXPECT_FALSE(sender.sender_clock());

    sender.set_sender_clock(true);
    EXPECT_TRUE(sender.sender_clock());
    EXPECT_TRUE(sender.media_clock().kind() == padl::sdp::MediaClockKind::sender);
    auto const session = *sender.session();
    EXPECT_EQ(session.session_version, before.session_version + 1);
    EXPECT_EQ(sender.info_block_version(), static_cast<uint8_t>(info_version + 1));
    std::string const text = padl::sdp::to_string(session);
    EXPECT_TRUE(text.find("a=mediaclk:sender\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("direct") == std::string::npos);
    // The reference the sender reports are in stays the grandmaster (RFC 7273 Section 4.8).
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=clock-domain:PTPv2 0\r\n") != std::string::npos);
    sender.set_sender_clock(true);
    EXPECT_EQ(sender.session()->session_version, session.session_version);

    // A direct receiver refuses it; one following the sender's clock accepts it and waits for an offset.
    auto const parsed = padl::sdp::parse(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->effective_media_clock(parsed->media[0])->kind() == padl::sdp::MediaClockKind::sender);
    Receiver direct{STEREO_L24, {}, transport};
    EXPECT_TRUE(direct.configure(*parsed).error() == Aes67Error::not_ptp_locked);
    Receiver foreign{STEREO_L24, {}, transport};
    foreign.set_sender_clock(true);
    EXPECT_TRUE(foreign.configure(*parsed).has_value());
    EXPECT_FALSE(foreign.offset().has_value());

    // A replacement sender keeps announcing it.
    Sender replacement{STEREO_L24, source.provider(), transport, "Foreign"};
    replacement.continue_from(sender);
    EXPECT_TRUE(replacement.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    EXPECT_TRUE(padl::sdp::to_string(*replacement.session()).find("a=mediaclk:sender\r\n") != std::string::npos);
}

TEST(aes67_sender, a_tag_switched_mid_run_leaves_no_sequence_or_timestamp_gap)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    auto const tag = ieee::make_vlan_tag(2, false, 3);
    uint64_t const base = (48'000ULL * 1'000'000) + 47;
    sender.tick(base, 0);
    EXPECT_TRUE(sender.set_vlan(tag));
    sender.tick(base + FPP, 0);
    sender.tick(base + (2 * FPP), 0);
    EXPECT_TRUE(sender.set_vlan(std::nullopt));
    sender.tick(base + (3 * FPP), 0);

    EXPECT_EQ(transport.queue.size(), size_t{4});
    std::array<bool, 4> const tagged{false, true, true, false};
    auto const first = padl::rtp::parse_rtp(transport.queue[0].bytes);
    for (size_t i = 0; i < transport.queue.size(); ++i) {
        auto const packet = padl::rtp::parse_rtp(transport.queue[i].bytes);
        EXPECT_EQ(transport.queue[i].vlan.has_value(), tagged[i]);
        EXPECT_EQ(packet->header.sequence.get(), static_cast<uint16_t>(first->header.sequence.get() + i));
        EXPECT_EQ(packet->header.timestamp.get(), static_cast<uint32_t>(first->header.timestamp.get() + (i * FPP)));
        EXPECT_EQ(packet->header.ssrc.get(), first->header.ssrc.get());
    }
    EXPECT_TRUE(transport.queue[1].vlan == std::optional{tag});
    EXPECT_EQ(sender.counters().lost, uint64_t{0});
}

TEST(aes67_sender, a_tag_is_kept_for_the_next_configure_and_a_refused_one_is_not)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    auto const tag = ieee::make_vlan_tag(2, false, 3);
    EXPECT_TRUE(sender.set_vlan(tag));  // not configured: kept
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    EXPECT_TRUE(transport.vlan == std::optional{tag});  // opened with it

    transport.refuse_tag = true;
    EXPECT_TRUE(sender.set_vlan(std::nullopt));
    auto const refused = sender.set_vlan(tag);
    EXPECT_TRUE(!refused && refused.error() == std::errc::operation_not_supported);
    EXPECT_FALSE(sender.vlan().has_value());
    EXPECT_FALSE(transport.vlan.has_value());
}

//
// ST 2110-30 and ST 2110-10, RFC 4607
//

// ST 2110-10's SDP and ST 2110-30 Section 6.2: ts-refclk with its domain and mediaclk:direct=0 at media level, the
// rtpmap, ptime and maxptime, and the channel order.
TEST(aes67_sender, an_st2110_sender_writes_the_attributes_st2110_asks_for)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_st2110(true);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004, .payload_type = 97}));
    sender.set_grandmaster(Grandmaster{.identity = GRANDMASTER.identity, .domain = 127});
    std::string const text = padl::sdp::to_string(*sender.session());
    auto const media = text.substr(text.find("m=audio"));
    for (auto const* const line :
         {"m=audio 5004 RTP/AVP 97\r\n",
          "a=rtpmap:97 L24/48000/2\r\n",
          "a=fmtp:97 channel-order=SMPTE2110.(U02)\r\n",
          "a=ptime:1\r\n",
          "a=maxptime:1\r\n",
          "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:127\r\n",
          "a=mediaclk:direct=0\r\n",
          "a=source-filter: incl IN IP4 239.69.1.2 192.168.1.10\r\n"}) {
        EXPECT_TRUE(media.find(line) != std::string::npos);
    }
    auto const parsed = padl::sdp::parse(text);
    EXPECT_EQ(parsed->media.at(0).channel_order, std::string{"SMPTE2110.(U02)"});
}

TEST(aes67_sender, a_named_channel_order_is_announced_one_session_version_on)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_st2110(true);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    auto const before = sender.session()->session_version;
    sender.set_channel_order("SMPTE2110.(ST)");
    EXPECT_EQ(sender.session()->session_version, before + 1);
    EXPECT_EQ(sender.session()->media.at(0).channel_order, std::string{"SMPTE2110.(ST)"});
    sender.set_channel_order("SMPTE2110.(51)");  // six channels of two: not described
    EXPECT_TRUE(sender.session()->media.at(0).channel_order.empty());
}

TEST(aes67_sender, an_aes67_sender_writes_neither_channel_order_nor_maxptime)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    std::string const text = padl::sdp::to_string(*sender.session());
    EXPECT_TRUE(text.find("a=fmtp:") == std::string::npos);
    EXPECT_TRUE(text.find("a=maxptime:") == std::string::npos);
}

// At 48 kHz the RTP timestamp wraps every 2^32 samples (89,478.485 s); 2^32 mod 48 is 16, so one packet spans it.
TEST(aes67_sender, a_sender_crosses_the_rtp_timestamp_wrap_without_a_gap)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    constexpr uint64_t WRAP = uint64_t{1} << 32U;
    constexpr uint64_t BASE = WRAP - 16 - (10 * FPP);  // on the grid, ten packets before the one spanning the wrap
    constexpr size_t PACKETS = 20;
    for (size_t i = 0; i < PACKETS; ++i) {
        sender.tick(BASE + (FPP - 1) + (i * FPP), 0);
    }
    EXPECT_EQ(transport.queue.size(), PACKETS);
    for (size_t i = 0; i < transport.queue.size(); ++i) {
        auto const packet = padl::rtp::parse_rtp(transport.queue[i].bytes);
        uint64_t const first = BASE + (i * FPP);
        EXPECT_EQ(packet->header.timestamp.get(), static_cast<uint32_t>(first));
        EXPECT_EQ(packet->payload[0], pattern(first, 0));
        auto const previous = i == 0 ? packet : padl::rtp::parse_rtp(transport.queue[i - 1].bytes);
        EXPECT_EQ(
            static_cast<uint16_t>(packet->header.sequence.get() - previous->header.sequence.get()),
            static_cast<uint16_t>(i == 0 ? 0 : 1));
    }
    EXPECT_EQ(sender.counters().packets, uint64_t{PACKETS});
}

// The attributes set are written at session level, one version on, the same session id; the same set changes nothing,
// and a replacement sender continues the session with them.
TEST(aes67_sender, extension_attributes_are_announced_one_session_version_on)
{
    LoopTransport transport;
    PatternSource source{8};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    auto const session_for = [](Sender const& s) { return s.session().value_or(padl::sdp::Session{}); };
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    auto const before = session_for(sender);
    std::vector<padl::sdp::Attribute> const attributes{{.name = "x-test-id", .value = "1234"}};
    sender.set_attributes(attributes);
    auto const after = session_for(sender);
    EXPECT_EQ(after.session_version, before.session_version + 1);
    EXPECT_EQ(after.session_id, before.session_id);
    EXPECT_TRUE(padl::sdp::to_string(after).find("t=0 0\r\na=x-test-id:1234\r\n") != std::string::npos);
    sender.set_attributes(attributes);
    EXPECT_EQ(session_for(sender).session_version, after.session_version);
    sender.set_attributes({});
    EXPECT_EQ(session_for(sender).session_version, after.session_version + 1);
    EXPECT_TRUE(padl::sdp::to_string(session_for(sender)).find("x-test-id") == std::string::npos);
    sender.set_attributes(attributes);
    Sender replacement{STEREO_L24, source.provider(), transport, "Test"};
    replacement.continue_from(sender);
    EXPECT_TRUE(replacement.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    EXPECT_TRUE(padl::sdp::to_string(session_for(replacement)).find("a=x-test-id:1234\r\n") != std::string::npos);
}

// A frame port's engine, run by a PacketSchedule from the SendRun the Sender gives it, sends in each tick the packets
// the packet path sends, with the same timestamps and payloads; missing frames are silence and a stall skips alike.
TEST(aes67_sender, a_frame_port_sends_what_the_packet_path_sends)
{
    TwoSenders senders{0, 18};
    EXPECT_TRUE(senders.configure(GROUP));
    EXPECT_TRUE(senders.frames.frame_tx != nullptr);
    uint64_t const base = (48'000ULL * 1'000'000) + 7;
    senders.packet_source.refuse_index = senders.frame_source.refuse_index = base + (20 * FPP) + 18 - (base % FPP);
    for (uint64_t now = base; now < base + 3000; now += 6) {
        senders.tick(now);
        EXPECT_EQ(senders.packets.queue.size(), senders.frames.queue.size());  // in the same tick
    }
    senders.tick(base + 3000 + (20 * uint64_t{FPP}));  // a stall: both skip ahead
    senders.expect_same_packets();
    EXPECT_TRUE(senders.packet.counters() == senders.frame.counters());
    EXPECT_EQ(senders.frame.counters().lost, uint64_t{FPP} + (19 * uint64_t{FPP}));  // a missing packet, then the skip
    auto const first = padl::rtp::parse_rtp(senders.frames.queue.front().bytes);
    auto const& runs = senders.frames.frame_tx->runs;
    EXPECT_EQ(runs.size(), size_t{1});
    EXPECT_TRUE(first && runs.size() == 1 && runs[0].ssrc == senders.frame.ssrc() && runs[0].send_offset == 6);
    EXPECT_TRUE(first && runs.size() == 1 && runs[0].first_sequence == first->header.sequence.get());
    EXPECT_TRUE(first && runs.size() == 1 && static_cast<uint32_t>(runs[0].first_index) == first->header.timestamp.get());
    EXPECT_EQ(runs[0].first_index % FPP, uint64_t{18});
    EXPECT_EQ(senders.frame.send_state().first_index, senders.packet.send_state().first_index);
    EXPECT_TRUE(senders.frame.state() == State::running);
}

// A frame port's run starts its start lead past the newest due packet, on the grid, so it can accept the run off the
// real-time thread; a pause stops it, and the next tick starts a new run under a new SSRC.
TEST(aes67_sender, a_frame_port_run_starts_its_lead_ahead_and_a_pause_stops_it)
{
    TwoSenders senders{100};
    EXPECT_TRUE(senders.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    senders.tick(base + 53);  // the packet path sends packet base
    auto const& tx = *senders.frames.frame_tx;
    EXPECT_EQ(tx.runs.size(), size_t{1});
    EXPECT_TRUE(senders.frames.queue.empty());
    EXPECT_EQ(tx.runs[0].first_index, base + (3 * uint64_t{FPP}));  // 100 frames, rounded up to the grid
    EXPECT_EQ(senders.frame.send_state().first_index, base + (3 * uint64_t{FPP}));
    EXPECT_FALSE(senders.frame.send_state().sent);
    for (uint64_t now = base + 59; now < base + 1000; now += 6) {
        senders.tick(now);
    }
    senders.expect_same_packets(3);  // the frame port's run starts three packets on
    auto const first = padl::rtp::parse_rtp(senders.frames.queue.front().bytes);
    EXPECT_TRUE(first && first->header.timestamp.get() == static_cast<uint32_t>(base + (3 * uint64_t{FPP})));
    EXPECT_TRUE(senders.frame.send_state().sent);

    uint32_t const ssrc = senders.frame.ssrc();
    senders.frame.pause();
    EXPECT_EQ(tx.stops, size_t{1});
    senders.frame.tick(base + 2000, 0);
    EXPECT_EQ(tx.runs.size(), size_t{2});
    EXPECT_TRUE(tx.runs.size() == 2 && tx.runs[1].ssrc != ssrc && tx.runs[1].ssrc == senders.frame.ssrc());
    EXPECT_TRUE(tx.runs.size() == 2 && tx.runs[1].first_index == base + (43 * uint64_t{FPP}));  // newest 40, three on
}

// A run held for its first report starts its engine where the packet path's first packet goes; a new send offset
// reaches the engine from the tick.
TEST(aes67_sender, a_frame_port_run_held_for_its_report_starts_where_the_packet_path_does)
{
    TwoSenders senders;
    EXPECT_TRUE(senders.configure(GROUP));
    auto const packet_hold = senders.packet.hold_reports();
    auto const frame_hold = senders.frame.hold_reports();
    uint64_t const base = 48'000ULL * 1'000'000;
    for (uint64_t now = base; now < base + 3000; now += 6) {
        senders.tick(now);
    }
    auto const& tx = *senders.frames.frame_tx;
    EXPECT_EQ(tx.runs.size(), size_t{1});
    EXPECT_TRUE(tx.runs.size() == 1 && tx.runs[0].first_index == senders.packet.send_state().first_index);
    EXPECT_TRUE(!senders.packets.queue.empty());
    senders.expect_same_packets();
    senders.packet.set_send_offset(12);
    senders.frame.set_send_offset(12);
    for (uint64_t now = base + 3000; now < base + 4000; now += 6) {
        senders.tick(now);
    }
    EXPECT_EQ(tx.send_offset, uint32_t{12});
    senders.expect_same_packets();
    EXPECT_TRUE(senders.packet.counters() == senders.frame.counters());
}

// A frame port with no room for a packet's frames: dropped, its frames still pulled and counted, as a packet the
// transport has no buffer for.
TEST(aes67_sender, a_frame_port_without_room_counts_what_a_packet_port_without_a_buffer_does)
{
    TwoSenders senders;
    EXPECT_TRUE(senders.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    senders.tick(base + 53);
    senders.packets.refuse = senders.frames.refuse = true;
    senders.packet_source.refuse_index = senders.frame_source.refuse_index = base + (2 * uint64_t{FPP});
    for (uint64_t now = base + 59; now < base + 300; now += 6) {
        senders.tick(now);
    }
    EXPECT_TRUE(senders.packet.counters() == senders.frame.counters());
    senders.expect_same_packets();  // the dropped ones left out on both paths
    EXPECT_EQ(senders.frame.counters().dropped, uint64_t{5});
    EXPECT_EQ(senders.frame.counters().lost, uint64_t{FPP});
    EXPECT_EQ(senders.packet_source.pulls, senders.frame_source.pulls);
}

// A frame port's error() is the sender's as a transport's send error is: State::error while it lasts, through a pause,
// ready again once it clears; the transport's own comes first, and a packet port's sender never asks a frame port.
TEST(aes67_sender, a_frame_ports_error_is_reported_as_the_transports_send_error_is)
{
    LoopTransport transport;
    transport.frame_ports = true;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    sender.tick(base + 47, 0);
    EXPECT_TRUE(sender.state() == State::running && !sender.error());
    transport.frame_error = std::make_error_code(std::errc::io_error);
    sender.tick(base + 95, 0);
    EXPECT_TRUE(sender.state() == State::error && sender.error() == std::errc::io_error);
    sender.tick(base + 143, 0);  // a packet written: still in error
    EXPECT_TRUE(sender.state() == State::error && sender.counters().packets == 3);
    transport.failing = std::error_code{EINVAL, std::system_category()};
    sender.tick(base + 143, 0);
    EXPECT_TRUE(sender.error() == std::errc::invalid_argument);  // the transport's first
    transport.failing.clear();
    sender.pause();
    EXPECT_TRUE(sender.state() == State::error && sender.error() == std::errc::io_error);  // paused, the port refusing
    transport.frame_error.clear();
    sender.pause();
    EXPECT_TRUE(sender.state() == State::idle && !sender.error());
    transport.frame_error = std::make_error_code(std::errc::io_error);
    sender.tick(base + 191, 0);
    EXPECT_TRUE(sender.state() == State::error);
    transport.frame_error.clear();
    sender.tick(base + 191, 0);  // nothing due, but the port sends again
    EXPECT_TRUE(sender.state() == State::ready && !sender.error());
    sender.tick(base + 239, 0);
    EXPECT_TRUE(sender.state() == State::running);

    LoopTransport packets;
    packets.frame_error = std::make_error_code(std::errc::io_error);
    Sender packet{STEREO_L24, source.provider(), packets, "Test"};
    packet.set_send_offset(0);
    EXPECT_TRUE(packet.configure(GROUP));
    packet.tick(base + 47, 0);
    EXPECT_TRUE(packet.state() == State::running && !packet.error() && packets.queue.size() == 1);
}

// The plan a transport is asked to open: the destination, its marking and tag, the format, payload type and grid.
TEST(aes67_sender, a_send_plan_carries_the_destination_format_and_grid)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_dscp(46);
    sender.set_packet_phase(30);
    EXPECT_TRUE(sender.set_vlan(statusbar::ieee::VlanTag{100, false, 5}));
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5006, .payload_type = 99, .ttl = 16}));
    EXPECT_EQ(transport.send_plans.size(), size_t{1});
    auto const& plan = transport.send_plans.back();
    EXPECT_TRUE(plan.destination == (Endpoint{.address = IPv4Address{239, 69, 1, 2}, .port = 5006}));
    EXPECT_TRUE(plan.ttl == 16 && plan.dscp == 46 && plan.vlan.has_value() && plan.vlan->get_vid() == 100);
    EXPECT_TRUE(plan.format == STEREO_L24 && plan.payload_type == 99 && plan.phase == 30);
    EXPECT_TRUE(
        plan ==
        (SendPlan{
            .destination = {.address = IPv4Address{239, 69, 1, 2}, .port = 5006},
            .ttl = 16,
            .dscp = 46,
            .vlan = statusbar::ieee::VlanTag{100, false, 5},
            .format = STEREO_L24,
            .payload_type = 99,
            .phase = 30,
        }));
    EXPECT_TRUE(transport.vlan.has_value());  // the default open_send_stream(): open_send() with the tag
}

// A send plan names the grandmaster the sender's ts-refclk names when it opens (none while it is not known), and a
// receiver of the SDP it announces plans the same one: for IEEE 1588 in a domain, and for IEEE 802.1AS, traceable.
TEST(aes67_sender, a_send_plan_names_the_grandmaster_its_receivers_plan)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(GROUP));
    EXPECT_FALSE(transport.send_plans.back().grandmaster.has_value());
    for (Grandmaster const grandmaster :
         {Grandmaster{.identity = GRANDMASTER.identity, .domain = 5},
          Grandmaster{
              .identity = GRANDMASTER.identity, .domain = 0, .version = padl::sdp::PtpVersion::ieee802_1as, .traceable = true}}) {
        sender.set_grandmaster(grandmaster);
        EXPECT_TRUE(sender.configure(GROUP));
        EXPECT_TRUE(transport.send_plans.back().grandmaster == std::optional{grandmaster});
        auto const announced = padl::sdp::parse(padl::sdp::to_string(*sender.session()));
        Receiver receiver{STEREO_L24, {}, transport};
        EXPECT_TRUE(announced && receiver.configure(*announced).has_value());
        EXPECT_TRUE(transport.receive_plans.back().grandmaster == std::optional{grandmaster});
    }
}

// The SendState an RTCP session reads is the packet path's, one engine step behind (the port sends after the tick that
// writes): a run's first tick publishes its first packet as not yet sent, never the port's run before, and a pause
// publishes nothing sending on both paths.
TEST(aes67_sender, a_frame_port_publishes_the_send_state_the_packet_path_does)
{
    TwoSenders senders;
    EXPECT_TRUE(senders.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    std::vector<SendState> packet;
    std::vector<SendState> frame;
    auto run = [&](uint64_t const from, uint64_t const to) {
        for (uint64_t now = from; now < to; now += 6) {
            senders.tick(now);
            packet.push_back(senders.packet.send_state());
            frame.push_back(senders.frame.send_state());
        }
    };
    run(base + 53, base + 1000);
    senders.packet.pause();
    senders.frame.pause();
    packet.push_back(senders.packet.send_state());
    frame.push_back(senders.frame.send_state());
    run(base + 2000, base + 3000);
    size_t fresh_runs = 0;
    size_t followed = 0;
    for (size_t t = 0; t < frame.size(); ++t) {
        auto const& b = frame[t];
        SendState const before = t > 0 ? packet[t - 1] : SendState{};
        if (b.ssrc == 0) {
            EXPECT_TRUE(b == SendState{} && packet[t] == SendState{});  // paused
        } else if (before.ssrc == 0 || before.first_index != packet[t].first_index) {
            EXPECT_TRUE(
                b == (SendState{.ssrc = b.ssrc, .first_index = packet[t].first_index, .next_index = packet[t].first_index}));
            ++fresh_runs;
        } else {
            EXPECT_TRUE(b.packets == before.packets && b.octets == before.octets && b.sent == before.sent);
            EXPECT_TRUE(b.first_index == before.first_index && b.next_index == before.next_index);
            ++followed;
        }
    }
    EXPECT_EQ(fresh_runs, size_t{2});
    EXPECT_TRUE(followed > 300);
    EXPECT_EQ(frame.back().ssrc, senders.frame.ssrc());
}

// A report hold that ends before its run's first packet: the packet path starts from the newest due packet, and the
// frame port's run, given the held one, is restarted there, so nothing counted as sent is left unsent.
TEST(aes67_sender, a_report_hold_ended_before_its_packet_restarts_the_frame_port_run)
{
    TwoSenders senders;
    EXPECT_TRUE(senders.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    {
        auto const packet_hold = senders.packet.hold_reports();
        auto const frame_hold = senders.frame.hold_reports();
        senders.tick(base + 53);
        senders.tick(base + 59);
    }
    senders.tick(base + 65);  // the restart: the port's progress is still the held run's, under the same SSRC
    auto const restarted = senders.frame.send_state();
    EXPECT_TRUE(restarted.next_index == restarted.first_index && restarted.packets == 0 && !restarted.sent);
    EXPECT_EQ(restarted.first_index, senders.packet.send_state().first_index);
    for (uint64_t now = base + 71; now < base + 3000; now += 6) {
        senders.tick(now);
    }
    auto const& tx = *senders.frames.frame_tx;
    auto const first = padl::rtp::parse_rtp(senders.packets.queue.at(0).bytes);
    EXPECT_EQ(tx.runs.size(), size_t{2});
    EXPECT_EQ(tx.stops, size_t{1});
    EXPECT_TRUE(first && tx.runs.size() == 2 && static_cast<uint32_t>(tx.runs[1].first_index) == first->header.timestamp.get());
    senders.expect_same_packets();
    EXPECT_TRUE(senders.packet.counters() == senders.frame.counters());
    EXPECT_EQ(senders.frame.send_state().first_index, senders.packet.send_state().first_index);
    EXPECT_EQ(tx.lost, uint64_t{0});
}

// A sender's frame port is its open port as a frame port, none for a packet port; by default it gathers nothing from
// other ports: an empty route set is accepted, any other refused and nothing changes.
TEST(aes67_sender, the_frame_port_gathers_only_what_is_written_by_default)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender packets{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(packets.configure(GROUP));
    EXPECT_TRUE(packets.frame_port() == nullptr);
    transport.frame_ports = true;
    Sender frames{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(frames.configure(GROUP));
    EXPECT_TRUE(frames.frame_port() != nullptr && frames.frame_port() == transport.frame_tx);
    EXPECT_TRUE(frames.frame_port()->gather_from({}).has_value());
    std::array const routes{FrameRoute{.source = {.device = 1, .buffer = 2}, .count = 2}};
    auto const refused = frames.frame_port()->gather_from(routes);
    EXPECT_TRUE(!refused && refused.error() == std::errc::operation_not_supported);
}

// What a frame port found missing in its run (SendProgress::lost) is counted lost once, as it grows; a reading behind
// the last counts nothing, and a new run's count starts from its own zero
TEST(aes67_sender, a_frame_ports_missing_frames_are_counted_lost)
{
    LoopTransport transport;
    transport.frame_ports = true;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    sender.tick(base + 47, 0);
    transport.frame_tx->send_due(base + 47);
    EXPECT_EQ(sender.counters().lost, uint64_t{0});
    transport.frame_lost = 12;
    sender.tick(base + 95, 0);
    EXPECT_EQ(sender.counters().lost, uint64_t{12});
    sender.tick(base + 143, 0);  // the same reading: nothing more
    EXPECT_EQ(sender.counters().lost, uint64_t{12});
    transport.frame_lost = 6;  // behind the last
    sender.tick(base + 191, 0);
    EXPECT_EQ(sender.counters().lost, uint64_t{12});
    transport.frame_lost = 18;
    sender.tick(base + 239, 0);
    EXPECT_EQ(sender.counters().lost, uint64_t{18});
}

// The sender's gather_from() reaches its open port, a packet port or a frame port, each by its own default; with none
// open, only an empty route set is accepted.
TEST(aes67_sender, gather_from_reaches_the_open_port_packet_or_frame)
{
    LoopTransport transport;
    PatternSource source{6};
    std::array const routes{FrameRoute{.source = {.device = 1, .buffer = 2}, .count = 2}};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.gather_from({}).has_value());
    auto const closed = sender.gather_from(routes);
    EXPECT_TRUE(!closed && closed.error() == std::errc::not_connected);
    EXPECT_TRUE(sender.configure(GROUP));
    auto const refused = sender.gather_from(routes);
    EXPECT_TRUE(!refused && refused.error() == std::errc::operation_not_supported);
    transport.packet_gather = true;
    EXPECT_TRUE(sender.gather_from(routes).has_value());
    EXPECT_TRUE(transport.gathered.size() == 1 && transport.gathered.front() == routes.front());
    transport.frame_ports = true;
    Sender frames{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(frames.configure(GROUP));
    auto const frame_refused = frames.gather_from(routes);
    EXPECT_TRUE(!frame_refused && frame_refused.error() == std::errc::operation_not_supported);
}

// A packet port that gathers from elsewhere: what it found missing in its run (SendProgress::lost) is counted lost once,
// as a frame port's is; before it gathers, its progress is not read.
TEST(aes67_sender, a_gathering_packet_ports_missing_frames_are_counted_lost)
{
    LoopTransport transport;
    transport.packet_gather = true;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    transport.frame_lost = 12;
    sender.tick(base + 47, 0);
    EXPECT_EQ(sender.counters().lost, uint64_t{0});  // not gathering: its progress not read
    std::array const routes{FrameRoute{.source = {.device = 1, .buffer = 2}, .count = 2}};
    EXPECT_TRUE(sender.gather_from(routes).has_value());
    sender.tick(base + 95, 0);
    EXPECT_EQ(sender.counters().lost, uint64_t{12});
    sender.tick(base + 143, 0);  // the same reading: nothing more
    EXPECT_EQ(sender.counters().lost, uint64_t{12});
    transport.frame_lost = 18;
    sender.tick(base + 191, 0);
    EXPECT_EQ(sender.counters().lost, uint64_t{18});
    EXPECT_EQ(sender.lost_elsewhere(), uint64_t{18});  // all of it the port's
    sender.reset_counters();
    EXPECT_TRUE(sender.counters().lost == 0 && sender.lost_elsewhere() == 0);
}

namespace {

/// The timestamps and sequences of what `transport` sent since `from`, in order.
auto sent_since(LoopTransport const& transport, size_t const from) -> std::vector<std::pair<uint32_t, uint16_t>>
{
    std::vector<std::pair<uint32_t, uint16_t>> sent;
    for (size_t i = from; i < transport.queue.size(); ++i) {
        auto const packet = padl::rtp::parse_rtp(transport.queue[i].bytes);
        sent.emplace_back(packet->header.timestamp.get(), packet->header.sequence.get());
    }
    return sent;
}

/// `sender` on a gathering packet port, ticked a packet at a time from `base` for `packets` packets.
void gather_and_send(Sender& sender, uint64_t const base, int const packets)
{
    std::array const routes{FrameRoute{.source = {.device = 1, .buffer = 2}, .count = 2}};
    EXPECT_TRUE(sender.gather_from(routes).has_value());
    for (int k = 0; k < packets; ++k) {
        sender.tick(base + FPP - 1 + (uint64_t{FPP} * static_cast<uint64_t>(k)), 0);
    }
}

}  // namespace

// A gathering packet port's packets that went nowhere are sent again from where it says, with the timestamps and
// sequences they had, at the next tick, before that tick's own; they are counted sent once
TEST(aes67_sender, a_gathering_packet_ports_packets_that_went_nowhere_are_sent_again)
{
    LoopTransport transport;
    transport.packet_gather = true;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    gather_and_send(sender, base, 10);
    auto const before = sent_since(transport, 0);
    uint64_t const packets = sender.counters().packets;
    uint64_t const next = sender.send_state().next_index;
    transport.resend = next - (3 * FPP);  // the last three went nowhere
    size_t const from = transport.queue.size();
    sender.tick(next + FPP - 1, 0);
    auto const again = sent_since(transport, from);
    EXPECT_EQ(again.size(), size_t{4});
    EXPECT_TRUE(again.size() == 4 && std::ranges::equal(std::span{again}.first(3), std::span{before}.last(3)));
    EXPECT_TRUE(
        again.size() == 4 && again[3].first == static_cast<uint32_t>(next) &&
        again[3].second == static_cast<uint16_t>(before.back().second + 1));
    EXPECT_TRUE(sender.counters().packets == packets + 1 && sender.counters().lost == 0);
}

// Further back than a burst, a resend goes from MAX_BURST - 1 packets behind the newest due packet, the frames before
// counted lost; a port not gathering is never asked
TEST(aes67_sender, a_resend_further_back_than_a_burst_is_bounded_and_counted_lost)
{
    LoopTransport transport;
    transport.packet_gather = true;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    gather_and_send(sender, base, 20);
    uint64_t const next = sender.send_state().next_index;
    uint64_t const packets = sender.counters().packets;
    transport.resend = next - (12 * FPP);
    size_t const from = transport.queue.size();
    sender.tick(next + FPP - 1, 0);  // the newest due is `next`: seven packets behind it, and it
    auto const again = sent_since(transport, from);
    EXPECT_TRUE(again.size() == Sender::MAX_BURST && again.front().first == static_cast<uint32_t>(next - (7 * FPP)));
    EXPECT_EQ(sender.counters().lost, uint64_t{5} * FPP);
    EXPECT_EQ(sender.lost_elsewhere(), uint64_t{5} * FPP);  // found where it sends, not in the provider's frames
    EXPECT_EQ(sender.counters().packets, packets - 5 + 1);  // the five too far back never went, nor are they sent
    LoopTransport plain;
    Sender unrouted{STEREO_L24, source.provider(), plain, "Test"};
    unrouted.set_send_offset(0);
    EXPECT_TRUE(unrouted.configure(GROUP));
    plain.resend = base;
    unrouted.tick(base + FPP - 1, 0);
    unrouted.tick(base + (2 * FPP) - 1, 0);
    EXPECT_TRUE(plain.resend_asks == 0 && plain.resend.has_value());
}

// A resend from before a resynchronisation's skip (the sender away): each frame missing counted lost once, those the skip
// counted and that now go again no longer lost, and the packets counted sent those that went
TEST(aes67_sender, a_resend_across_a_resync_counts_each_missing_frame_once)
{
    LoopTransport transport;
    transport.packet_gather = true;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    gather_and_send(sender, base, 10);
    uint64_t const n10 = sender.send_state().next_index;
    sender.tick(n10 + (21 * FPP) - 1, 0);  // away 20 packets: skipped, counted lost; the newest sent
    EXPECT_EQ(sender.counters().lost, uint64_t{20} * FPP);
    transport.resend = n10 - (2 * FPP);  // the last two before the skip went nowhere
    sender.tick(n10 + (22 * FPP) - 1, 0);
    EXPECT_EQ(sender.counters().lost, uint64_t{16} * FPP);  // n10 - 2 to n10 + 13: the rest goes again
    EXPECT_EQ(sender.counters().packets, uint64_t{16});     // 8 before, 8 now: n10 + 14 to n10 + 21
}

// A packet port is told the send offset at the sender's first tick that sends, and again only when it changes
TEST(aes67_sender, a_packet_port_is_told_each_send_offset_once)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_send_offset(6);
    EXPECT_TRUE(sender.configure(GROUP));
    uint64_t const base = 48'000ULL * 1'000'000;
    sender.tick(base + 47, 0);
    sender.tick(base + 95, 0);
    EXPECT_TRUE(transport.send_offsets == std::vector<uint32_t>({6}));
    sender.set_send_offset(48);
    sender.tick(base + 191, 0);
    sender.tick(base + 239, 0);
    EXPECT_TRUE(transport.send_offsets == std::vector<uint32_t>({6, 48}));
}

TEST_MAIN(padl_aes67, aes67_sender_test)
