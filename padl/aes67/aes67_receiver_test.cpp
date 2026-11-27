// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/aes67/aes67_receiver.hpp"

#include "padl/aes67/aes67_error.hpp"
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
#include <map>
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

/// Every byte of `block` is the pattern for its index.
inline void expect_pattern(Received const& block)
{
    for (size_t i = 0; i < block.bytes.size(); ++i) {
        EXPECT_EQ(block.bytes[i], pattern(block.first_index + (i / 6), i % 6));
    }
}

/// Each frame a sink was handed, by its index (the last one handed for an index).
auto frames_by_index(RecordingSink const& sink, size_t const frame_bytes) -> std::map<uint64_t, std::vector<uint8_t>>
{
    std::map<uint64_t, std::vector<uint8_t>> frames;
    for (auto const& block : sink.blocks) {
        for (size_t f = 0; f < block.frames; ++f) {
            auto const first = block.bytes.begin() + static_cast<std::ptrdiff_t>(f * frame_bytes);
            frames[block.first_index + f] = {first, first + static_cast<std::ptrdiff_t>(frame_bytes)};
        }
    }
    return frames;
}

/// One stream received twice from the same packets: through a packet port, and through a frame port that runs the
/// Depacketizer beside the network.
struct TwoPaths
{
    explicit TwoPaths(bool const sender_clock = false)
    {
        frames.frame_ports = true;
        packet.set_sender_clock(sender_clock);
        frame.set_sender_clock(sender_clock);
    }
    /// Both configured for `session`.
    auto configure(padl::sdp::Session const& session) -> bool
    {
        return packet.configure(session).has_value() && frame.configure(session).has_value();
    }
    /// The datagram to both.
    void push(Endpoint const& destination, std::vector<uint8_t> const& bytes)
    {
        packets.queue.push_back({destination, bytes});
        frames.queue.push_back({destination, bytes});
    }
    /// Both drained at `now_index`.
    void drain(uint64_t const now_index)
    {
        packet.drain(now_index);
        frame.drain(now_index);
    }
    /// The two hand on the same frames, and count, publish and report alike.
    void expect_same() const
    {
        EXPECT_TRUE(frames_by_index(packet_sink, 6) == frames_by_index(frame_sink, 6));
        EXPECT_TRUE(packet.counters() == frame.counters());
        EXPECT_TRUE(packet.receive_state() == frame.receive_state());
        EXPECT_TRUE(packet.ssrc() == frame.ssrc());
        EXPECT_TRUE(packet.state() == frame.state());
        EXPECT_TRUE(packet.offset() == frame.offset());
    }

    LoopTransport packets;
    LoopTransport frames;
    RecordingSink packet_sink;
    RecordingSink frame_sink;
    Receiver packet{STEREO_L24, packet_sink.handler(), packets};
    Receiver frame{STEREO_L24, frame_sink.handler(), frames};
};

/// RTP packet `seq` of the pattern at `index`, its timestamp carrying `offset`.
auto offset_packet(
    uint16_t const seq, uint64_t const index, uint32_t const offset, uint32_t const ssrc = 0x1234, uint8_t const pt = 97)
    -> std::vector<uint8_t>
{
    auto bytes = rtp_packet(seq, static_cast<uint32_t>(index), ssrc, pt);
    padl::rtp::RtpHeader header{};
    header.init(pt, seq, static_cast<uint32_t>(index) + offset, ssrc);
    (void)store_unchecked(std::span{bytes}, header);
    return bytes;
}

}  // namespace

TEST(aes67_receiver, a_receiver_following_the_senders_clock_refuses_a_direct_stream)
{
    LoopTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    receiver.set_sender_clock(true);
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)).error() == Aes67Error::not_sender_clocked);
    EXPECT_TRUE(receiver.configure(stream_session(group, std::nullopt)).error() == Aes67Error::not_sender_clocked);
    auto other = stream_session(group, std::nullopt);
    other.media[0].media_clock = padl::sdp::MediaClock{.other = true};  // e.g. IEEE1722=<stream id>
    EXPECT_TRUE(receiver.configure(other).error() == Aes67Error::not_sender_clocked);
    EXPECT_FALSE(receiver.configured());

    // A direct receiver's offset is the SDP's.
    Receiver direct{STEREO_L24, {}, transport};
    EXPECT_TRUE(direct.configure(stream_session(group, 0x1000)).has_value());
    EXPECT_TRUE(direct.offset() == std::optional<uint32_t>{0x1000});
}

// A port that cannot deliver its group (an XDP filter without its rule) puts the receiver in error from the next
// drain, with the port's reason; once the port says nothing it is ready, its stream afresh, and running from its
// next packet.
TEST(aes67_receiver, a_port_error_holds_the_receiver_in_error_until_it_clears)
{
    LoopTransport transport;
    transport.every_tick = true;  // as XdpTransport, the one whose ports report errors
    Receiver receiver{STEREO_L24, {}, transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)).has_value());
    transport.queue.push_back({group, rtp_packet(1, 952, 0x1234, 97)});
    receiver.drain(952);
    EXPECT_TRUE(receiver.state() == State::running && receiver.ssrc().has_value());
    transport.port_error = make_error_code(std::errc::no_buffer_space);
    receiver.drain(1000);
    EXPECT_TRUE(receiver.state() == State::error && receiver.error() == std::errc::no_buffer_space);
    transport.port_error.clear();
    receiver.drain(1048);
    EXPECT_TRUE(receiver.state() == State::ready && !receiver.error() && !receiver.ssrc().has_value());
    transport.queue.push_back({group, rtp_packet(1, 1096, 0x1234, 97)});
    receiver.drain(1096);
    EXPECT_TRUE(receiver.state() == State::running);
}

TEST(aes67_receiver, refuses_streams_it_cannot_accept)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};

    EXPECT_TRUE(receiver.configure(stream_session(group, std::nullopt)).error() == Aes67Error::not_ptp_locked);
    auto session = stream_session(group);
    session.media[0].channels = 8;
    EXPECT_TRUE(receiver.configure(session).error() == Aes67Error::format_mismatch);
    session = stream_session(group);
    session.media[0].sample_rate = 44100;
    EXPECT_TRUE(receiver.configure(session).error() == Aes67Error::unsupported_format);
    session = stream_session(group);
    session.media[0].encoding = padl::sdp::Encoding::am824;
    EXPECT_TRUE(receiver.configure(session).error() == Aes67Error::encoding_mismatch);
    EXPECT_TRUE(receiver.configure(stream_session(group), 1).error() == Aes67Error::no_media);
    EXPECT_FALSE(receiver.configured());
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    EXPECT_TRUE(receiver.configured());
    EXPECT_TRUE(receiver.state() == State::ready);
}

TEST(aes67_receiver, accepts_the_offered_format_in_its_own_encoding)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L32, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};

    // only L24 offered: refused for its sample width; an L24 receiver refuses L32 alike
    auto session = stream_session(group);
    EXPECT_TRUE(receiver.configure(session).error() == Aes67Error::encoding_mismatch);
    session.media[0].encoding = padl::sdp::Encoding::l32;
    Receiver l24{STEREO_L24, sink.handler(), transport};
    EXPECT_TRUE(l24.configure(session).error() == Aes67Error::encoding_mismatch);

    // L24 first, L32 as payload type 96: the L32 receiver accepts 96, not 97
    session = stream_session(group);
    EXPECT_TRUE(session.media[0].add_alternative(
        {.payload_type = 96, .encoding = padl::sdp::Encoding::l32, .sample_rate = 48000, .channels = 2}));
    EXPECT_TRUE(receiver.configure(session));
    uint64_t const index = (1ULL << 32U) + 96;
    auto send = [&](uint8_t const pt, size_t const frame_bytes) {
        std::vector<uint8_t> const payload(size_t{FPP} * frame_bytes, uint8_t{0x5A});
        padl::rtp::RtpHeader header{};
        header.init(pt, 10, static_cast<uint32_t>(index), 0x1234);
        std::vector<uint8_t> bytes(padl::rtp::RtpHeader::LENGTH + payload.size());
        (void)padl::rtp::serialize_rtp(header, payload, bytes);
        transport.queue.push_back({group, std::move(bytes)});
    };
    send(97, 6);
    send(96, 8);
    receiver.drain(index + FPP);
    EXPECT_EQ(sink.blocks.size(), size_t{1});
    EXPECT_TRUE(sink.blocks.size() == 1 && sink.blocks[0].frames == FPP && sink.blocks[0].bytes.size() == size_t{FPP} * 8);
}

TEST(aes67_receiver, pushes_frames_by_index_and_counts_loss_reorder_and_strays)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    uint32_t const offset = 0x1000;
    EXPECT_TRUE(receiver.configure(stream_session(group, offset)));

    // Indices just past 2^32 so the timestamps wrap: the reference unwraps them.
    uint64_t const base = (1ULL << 32U) + 96;
    auto send = [&](uint16_t const seq, uint64_t const index, uint32_t const ssrc = 0x1234, uint8_t const pt = 97) {
        auto bytes = rtp_packet(seq, static_cast<uint32_t>(index), ssrc, pt);
        // Timestamps carry the offset; the pattern is by index.
        padl::rtp::RtpHeader header{};
        header.init(pt, seq, static_cast<uint32_t>(index) + offset, ssrc);
        (void)store_unchecked(std::span{bytes}, header);
        transport.queue.push_back({group, std::move(bytes)});
    };
    auto at = [&](uint16_t const n) { return base + (uint64_t{n} * FPP); };

    send(10, at(0));  // on probation, handed on all the same
    send(11, at(1));
    send(12, at(2));
    send(14, at(4));                                   // 13 missing
    send(13, at(3));                                   // ...arrives late
    send(13, at(3));                                   // duplicate
    send(16, at(6));                                   // 15 lost for good
    send(17, at(7), 0x9999);                           // another source
    send(17, at(7), 0x1234, 96);                       // another payload type
    transport.queue.push_back({group, {0x80, 0x61}});  // malformed
    receiver.drain(at(7));

    EXPECT_EQ(sink.blocks.size(), size_t{6});
    std::array<uint16_t, 6> const order{0, 1, 2, 4, 3, 6};
    for (size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ(sink.blocks[i].first_index, at(order[i]));
        EXPECT_EQ(sink.blocks[i].frames, FPP);
        expect_pattern(sink.blocks[i]);
    }
    auto const& c = receiver.counters();
    EXPECT_EQ(c.packets, uint64_t{6});
    EXPECT_EQ(c.frames, uint64_t{6 * FPP});
    EXPECT_EQ(c.lost, uint64_t{FPP});   // 15; 13 was recovered
    EXPECT_EQ(c.late, uint64_t{FPP});   // 13
    EXPECT_EQ(c.dropped, uint64_t{4});  // duplicate, source, payload type, malformed
    EXPECT_EQ(receiver.ssrc(), std::optional<uint32_t>{0x1234});
    EXPECT_TRUE(receiver.state() == State::running);

    // Reset: the counters start again; the source and its sequence are kept, so the next packet
    // is placed as before, the gap behind it (17) is a loss and a repeat is still a duplicate.
    receiver.reset_counters();
    EXPECT_TRUE(receiver.counters() == Counters{});
    send(18, at(8));
    send(18, at(8));
    receiver.drain(at(8));
    EXPECT_EQ(receiver.ssrc(), std::optional<uint32_t>{0x1234});
    EXPECT_EQ(receiver.counters().packets, uint64_t{1});
    EXPECT_EQ(receiver.counters().lost, uint64_t{FPP});
    EXPECT_EQ(receiver.counters().dropped, uint64_t{1});
    EXPECT_EQ(sink.blocks.back().first_index, at(8));

    // A second of silence from 0x1234, and a new source is adopted.
    uint64_t const later = at(7) + 48000;
    send(500, later, 0x5555);
    send(501, later + FPP, 0x5555);
    receiver.drain(later + FPP);
    EXPECT_EQ(receiver.ssrc(), std::optional<uint32_t>{0x5555});
    EXPECT_EQ(sink.blocks.back().first_index, later + FPP);
}

TEST(aes67_receiver, each_refused_packet_is_counted_under_its_reason)
{
    LoopTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    auto session = stream_session(group);
    session.source_filter = padl::sdp::SourceFilter{.destination = std::nullopt, .source = transport.source.address};
    EXPECT_TRUE(receiver.configure(session));
    uint64_t const base = 48'000ULL * 4000;
    auto send = [&](uint16_t const n, uint32_t const ssrc = 0x1234, uint8_t const pt = 97) {
        transport.queue.push_back({group, rtp_packet(n, static_cast<uint32_t>(base + (uint64_t{n} * FPP)), ssrc, pt)});
    };
    send(0);
    send(1);
    send(1);                                                                        // sequence: a duplicate
    send(2, 0x9999);                                                                // ssrc: another source while 0x1234 is live
    send(2, 0x1234, 96);                                                            // payload type
    transport.queue.push_back({group, {0x80, 0x61}});                               // not RTP
    transport.queue.push_back({group, {0x80, 200, 0x00, 0x06, 0, 0, 0x12, 0x34}});  // RTCP SR (RFC 5761): not refused
    auto empty = rtp_packet(2, static_cast<uint32_t>(base + (uint64_t{2} * FPP)));
    empty.resize(padl::rtp::RtpHeader::LENGTH);
    transport.queue.push_back({group, empty});  // size: no frames
    auto ragged = rtp_packet(2, static_cast<uint32_t>(base + (uint64_t{2} * FPP)));
    ragged.pop_back();
    transport.queue.push_back({group, ragged});  // size: not whole frames
    receiver.drain(base + (uint64_t{2} * FPP));
    transport.source.address = IPv4Address{192, 168, 1, 99};
    send(2);  // source: not the filter's
    receiver.drain(base + (uint64_t{3} * FPP));

    auto const& c = receiver.counters();
    EXPECT_EQ(c.packets, uint64_t{2});
    EXPECT_EQ(c.drops.sequence, uint64_t{1});
    EXPECT_EQ(c.drops.ssrc, uint64_t{1});
    EXPECT_EQ(c.drops.payload_type, uint64_t{1});
    EXPECT_EQ(c.drops.not_rtp, uint64_t{1});
    EXPECT_EQ(c.drops.size, uint64_t{2});
    EXPECT_EQ(c.drops.source, uint64_t{1});
    EXPECT_EQ(c.drops.no_offset, uint64_t{0});
    EXPECT_EQ(c.dropped, uint64_t{7});
}

TEST(aes67_receiver, a_sender_clocked_stream_counts_packets_before_its_offset_as_no_offset)
{
    LoopTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    receiver.set_sender_clock(true);
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    auto session = stream_session(group, std::nullopt);
    session.media[0].media_clock = padl::sdp::MediaClock::sender();
    EXPECT_TRUE(receiver.configure(session));
    uint64_t const base = 48'000ULL * 5000;
    transport.queue.push_back({group, rtp_packet(0, static_cast<uint32_t>(base))});
    transport.queue.push_back({group, rtp_packet(1, static_cast<uint32_t>(base + FPP))});
    receiver.drain(base + FPP);
    EXPECT_EQ(receiver.counters().drops.no_offset, uint64_t{2});
    EXPECT_EQ(receiver.counters().dropped, uint64_t{2});
    receiver.set_offset(0);
    transport.queue.push_back({group, rtp_packet(2, static_cast<uint32_t>(base + (uint64_t{2} * FPP)))});
    receiver.drain(base + (uint64_t{2} * FPP));
    EXPECT_EQ(receiver.counters().packets, uint64_t{1});
    EXPECT_EQ(receiver.counters().dropped, uint64_t{2});
}

TEST(aes67_receiver, accepts_an_eight_channel_stream_in_packets_of_up_to_its_ptime)
{
    // One sender's announcement: source port = destination port, a large media clock offset.
    constexpr char const* SDP_TEXT =
        "v=0\r\no=dmixv2 37878419 1793158514 IN IP4 10.10.32.163\r\ns=dmixv2_01\r\nc=IN IP4 239.200.163.140/32\r\n"
        "t=0 0\r\nm=audio 5004 RTP/AVP 99\r\ni=8 channels\r\na=recvonly\r\na=rtpmap:99 L24/48000/8\r\na=ptime:1\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-10-6C-16:0\r\na=mediaclk:direct=4045604396\r\n";
    constexpr StreamFormat EIGHT_L24{
        .encoding = padl::sdp::Encoding::l24, .channels = 8, .sample_rate = 48000, .frames_per_packet = FPP};
    LoopTransport transport;
    transport.source = {.address = IPv4Address{10, 10, 32, 163}, .port = 5004};
    RecordingSink sink;
    Receiver receiver{EIGHT_L24, sink.handler(), transport};
    auto const session = padl::sdp::parse(SDP_TEXT);
    EXPECT_TRUE(session && receiver.configure(*session));
    Endpoint const group{.address = IPv4Address{239, 200, 163, 140}, .port = 5004};

    // 48 frames (a 1206-octet frame), then 24, then 12; Sender::MAX_PAYLOAD's 60 is more than the 1 ms ptime.
    uint64_t const base = (1ULL << 40U) + 12345;
    std::array<uint32_t, 4> const sizes{FPP, FPP / 2, FPP / 4, Sender::MAX_PAYLOAD / 24};
    uint64_t index = base;
    for (uint16_t i = 0; i < sizes.size(); ++i) {
        padl::rtp::RtpHeader header{};
        header.init(99, static_cast<uint16_t>(0xCA34 + i), static_cast<uint32_t>(index) + 4045604396U, 0x0241FA93);
        std::vector<uint8_t> bytes(padl::rtp::RtpHeader::LENGTH + (size_t{sizes[i]} * 24));
        (void)padl::rtp::serialize_rtp_header(header, bytes.size() - padl::rtp::RtpHeader::LENGTH, bytes);
        transport.queue.push_back({group, std::move(bytes)});
        index += sizes[i];
    }
    receiver.drain(index + 53);

    EXPECT_EQ(sink.blocks.size(), sizes.size() - 1);
    EXPECT_EQ(receiver.counters().drops.size, uint64_t{1});
    EXPECT_EQ(receiver.counters().dropped, uint64_t{1});
    index = base;
    for (size_t i = 0; i < std::min(sizes.size(), sink.blocks.size()); ++i) {
        EXPECT_EQ(sink.blocks[i].first_index, index);
        EXPECT_EQ(sink.blocks[i].frames, sizes[i]);
        index += sizes[i];
    }
}

TEST(aes67_receiver, the_ssrc_from_another_address_is_refused_while_the_learnt_source_is_live)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    transport.every_tick = true;  // each packet is received in its own drain, from the address it was sent from
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 6000;
    auto at = [&](uint16_t const n) { return base + (uint64_t{n} * FPP); };
    auto send = [&](uint16_t const n, uint16_t const seq) {
        transport.queue.push_back({group, rtp_packet(seq, static_cast<uint32_t>(at(n)))});
        receiver.drain(at(n));
    };
    send(0, 0);
    send(1, 1);
    EXPECT_TRUE(receiver.receive_state().source == transport.source.address);

    // Another host copying the SSRC, its sequence far on: refused, the stream's sequence untouched.
    transport.source.address = IPv4Address{192, 168, 1, 66};
    send(2, 30'000);
    send(3, 30'001);
    EXPECT_EQ(receiver.counters().drops.source, uint64_t{2});
    EXPECT_EQ(receiver.receive_state().extended_max, uint32_t{1});
    transport.source.address = IPv4Address{192, 168, 1, 10};
    send(4, 2);
    EXPECT_EQ(receiver.counters().packets, uint64_t{3});
    EXPECT_EQ(receiver.counters().drops.sequence, uint64_t{0});
    EXPECT_EQ(receiver.receive_state().extended_max, uint32_t{2});

    // After a second of silence the sender at a new address is adopted.
    transport.source.address = IPv4Address{192, 168, 1, 11};
    send(1004, 3);
    EXPECT_TRUE(receiver.receive_state().source == (IPv4Address{192, 168, 1, 11}));
    EXPECT_EQ(receiver.counters().packets, uint64_t{4});
}

// A gap counts its frames and a wrap from 65535 to 0 nothing; the learnt source resuming after a silence past
// RtpSequence::MAX_DROPOUT packets (restarted, its sequence anywhere) counts nothing, as after a resubscription.
TEST(aes67_receiver, a_source_resuming_after_a_long_silence_counts_nothing_lost)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    transport.every_tick = true;  // each packet is received in its own drain, at its own index
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 9000;
    auto send = [&](uint64_t const n, uint16_t const seq) {
        transport.queue.push_back({group, rtp_packet(seq, static_cast<uint32_t>(base + (n * FPP)))});
        receiver.drain(base + (n * FPP));
    };
    send(0, 65533);
    send(1, 65534);
    send(3, 0);  // 65535 lost, across the wrap
    send(4, 1);
    EXPECT_EQ(receiver.counters().lost, uint64_t{FPP});
    uint64_t const resumed = 5 + padl::rtp::RtpSequence::MAX_DROPOUT;
    send(resumed, 500);  // a step of 499 had counted 498 packets
    send(resumed + 1, 501);
    send(resumed + 3, 503);
    EXPECT_EQ(receiver.counters().lost, uint64_t{2 * FPP});
    EXPECT_EQ(receiver.counters().packets, uint64_t{7});
    EXPECT_TRUE(receiver.configure(stream_session(group)));  // resubscribed
    send(resumed + 4, 40'000);
    send(resumed + 5, 40'001);
    EXPECT_EQ(receiver.counters().lost, uint64_t{2 * FPP});
    EXPECT_EQ(receiver.counters().packets, uint64_t{9});
}

// Two sequences 2900 apart under the learnt source, in turn five packets each: each switch to the one ahead is a step
// RtpSequence accepts, counted lost, but the one behind is confirmed again at once, so that step was not loss.
TEST(aes67_receiver, two_interleaved_sequences_of_one_source_count_no_loss_that_stays)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    transport.every_tick = true;
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 9000;
    uint64_t n = 0;
    auto send = [&](uint16_t const ahead) {
        auto const seq = static_cast<uint16_t>(1000 + n + ahead);
        transport.queue.push_back({group, rtp_packet(seq, static_cast<uint32_t>(base + (n * FPP)))});
        receiver.drain(base + (n * FPP));
        ++n;
    };
    for (int k = 0; k < 10; ++k) {
        send(0);
    }
    for (int round = 0; round < 10; ++round) {
        for (int k = 0; k < 5; ++k) {
            send(2900);
        }
        EXPECT_TRUE(receiver.counters().lost <= uint64_t{2900} * FPP);
        for (int k = 0; k < 5; ++k) {
            send(0);
        }
        EXPECT_EQ(receiver.counters().lost, uint64_t{0});
    }
    EXPECT_EQ(receiver.counters().drops.sequence, uint64_t{10});  // each switch back's first packet
    EXPECT_EQ(receiver.counters().packets, uint64_t{n - 10});
}

// A new source's packets on probation (RFC 3550 Appendix A.1) are handed on, a gap among them counted nothing lost.
TEST(aes67_receiver, a_gap_on_probation_counts_nothing_lost)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    transport.every_tick = true;
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 9000;
    for (uint16_t const seq : {10, 12, 13, 15}) {
        transport.queue.push_back({group, rtp_packet(seq, static_cast<uint32_t>(base + (uint64_t{seq} * FPP)))});
        receiver.drain(base + (uint64_t{seq} * FPP));
    }
    EXPECT_EQ(receiver.counters().packets, uint64_t{4});
    EXPECT_EQ(receiver.counters().lost, uint64_t{FPP});  // 14 alone: 11 went missing on probation
}

// A jump of 2900 the sequence then follows for RtpSequence::MAX_MISORDER packets was loss: a later confirmed jump
// leaves it counted.
TEST(aes67_receiver, a_jump_the_sequence_follows_on_from_stays_lost)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    transport.every_tick = true;
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 9000;
    uint64_t n = 0;
    auto send = [&](uint16_t const seq) {
        transport.queue.push_back({group, rtp_packet(seq, static_cast<uint32_t>(base + (n * FPP)))});
        receiver.drain(base + (n * FPP));
        ++n;
    };
    for (uint16_t s = 0; s < 10; ++s) {
        send(s);
    }
    for (uint16_t s = 2910; s < 2910 + padl::rtp::RtpSequence::MAX_MISORDER + 1; ++s) {
        send(s);  // 2900 lost, then the sequence goes on
    }
    send(20'000);
    send(20'001);  // a jump RtpSequence confirms
    EXPECT_EQ(receiver.counters().lost, uint64_t{2900} * FPP);
}

// Genuine loss survives a jump RtpSequence confirms back: steps of 891 and 1491 stay lost when the sequence
// goes back before the second; one back into the second's gap withdraws that step's loss alone, less what arrived late.
TEST(aes67_receiver, a_confirmed_jump_back_withdraws_only_the_newest_step_not_recovered)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    transport.every_tick = true;
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 9000;
    uint64_t n = 0;
    auto send = [&](uint16_t const seq) {
        transport.queue.push_back({group, rtp_packet(seq, static_cast<uint32_t>(base + (n * FPP)))});
        receiver.drain(base + (n * FPP));
        ++n;
    };
    auto run = [&](uint16_t const from, uint16_t const to) {
        for (uint16_t s = from; s <= to; ++s) {
            send(s);
        }
    };
    run(0, 9);
    run(900, 909);    // 890 lost
    run(2400, 2409);  // 1490 lost
    send(905);
    send(906);  // confirmed: before the newest step, so both stay lost
    EXPECT_EQ(receiver.counters().lost, uint64_t{890 + 1490} * FPP);
    run(907, 916);
    run(1300, 1309);  // 383 lost
    run(1440, 1449);  // 130 lost: the newest step
    for (uint16_t s = 1350; s < 1440; ++s) {
        send(s);  // 90 of its gap late, not withdrawn again
    }
    send(1312);
    send(1313);  // confirmed back into the newest step's gap: its 40 not recovered withdrawn
    EXPECT_EQ(receiver.counters().lost, uint64_t{890 + 1490 + 383} * FPP);
}

TEST(aes67_receiver, a_packet_of_more_frames_than_the_maxptime_is_refused)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    auto session = stream_session(group);
    session.media[0].packet_time_us = 125;
    session.media[0].max_packet_time_us = 1000;
    EXPECT_TRUE(receiver.configure(session));
    uint64_t const base = 48'000ULL * 7000;
    auto packet = [&](uint16_t const seq, size_t const frames) {
        padl::rtp::RtpHeader header{};
        header.init(97, seq, static_cast<uint32_t>(base + (uint64_t{seq} * FPP)), 0x1234);
        std::vector<uint8_t> bytes(padl::rtp::RtpHeader::LENGTH + (frames * 6));
        (void)padl::rtp::serialize_rtp_header(header, bytes.size() - padl::rtp::RtpHeader::LENGTH, bytes);
        transport.queue.push_back({group, std::move(bytes)});
    };
    packet(0, FPP);
    packet(1, FPP + 1);
    packet(2, 240);  // Sender::MAX_PAYLOAD's worth: five times the frames
    receiver.drain(base + (uint64_t{3} * FPP));
    EXPECT_EQ(sink.blocks.size(), size_t{1});
    EXPECT_EQ(receiver.counters().drops.size, uint64_t{2});
    EXPECT_EQ(receiver.counters().frames, uint64_t{FPP});
}

TEST(aes67_receiver, accepts_late_packets_as_far_back_as_rtp_sequence_does)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 2000;
    auto send = [&](uint16_t const n) {
        transport.queue.push_back({group, rtp_packet(n, static_cast<uint32_t>(base + (uint64_t{n} * FPP)))});
    };
    // 0..200 except 136 and 101, then those two: 64 and 99 behind the high-water mark 200.
    for (uint16_t n = 0; n <= 200; ++n) {
        if (n != 136 && n != 101 && n != 100) {
            send(n);
        }
    }
    while (!transport.queue.empty()) {
        receiver.drain(base + (201 * FPP));  // a drain reads at most MAX_BATCHES batches
    }
    auto const lost = receiver.counters().lost;
    EXPECT_EQ(lost, uint64_t{3 * FPP});
    send(136);                           // 64 behind
    send(101);                           // 99 behind: RtpSequence's last late packet
    send(100);                           // 100 behind: RtpSequence reads it as a jump
    receiver.drain(base + (202 * FPP));  // a packet time on: past the due-tick gate
    EXPECT_EQ(receiver.counters().late, uint64_t{2 * FPP});
    EXPECT_EQ(receiver.counters().lost, uint64_t{FPP});
    EXPECT_EQ(receiver.counters().dropped, uint64_t{1});
    EXPECT_EQ(sink.blocks.back().first_index, base + (101 * FPP));
}

TEST(aes67_receiver, polls_a_queuing_port_only_when_a_packet_can_be_due)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 3000;  // a multiple of 48
    auto send = [&](uint16_t const n) {
        transport.queue.push_back({group, rtp_packet(n, static_cast<uint32_t>(base + (uint64_t{n} * FPP)))});
    };
    // Before the first packet: every tick.
    for (uint64_t t = 0; t < 4; ++t) {
        receiver.drain(base + (t * 6));
    }
    EXPECT_EQ(transport.receives, size_t{4});
    // Packet n, [base + 48n, base + 48(n + 1)), arrives a tick after its last frame.
    size_t polls_at_start = 0;
    for (uint16_t n = 0; n < 10; ++n) {
        if (n == 2) {
            polls_at_start = transport.receives;
        }
        uint64_t const arrival = base + (uint64_t{n + 1U} * FPP) + 6;
        for (uint64_t now = arrival - 48; now < arrival; now += 6) {
            receiver.drain(now);
        }
        send(n);
        receiver.drain(arrival);
    }
    EXPECT_EQ(receiver.counters().packets, uint64_t{10});
    // In steady state: from a tick before the next packet can be sent to the
    // tick it arrives, three polls per eight ticks.
    EXPECT_EQ(transport.receives - polls_at_start, size_t{8 * 3});

    // A lost packet: polled every tick past due until the next one.
    size_t const before = transport.receives;
    uint64_t const due = base + (11 * FPP) - 6;
    for (uint64_t now = due; now < due + 96; now += 6) {
        receiver.drain(now);
    }
    EXPECT_EQ(transport.receives - before, size_t{16});

    // A transport whose ports keep a tick's datagrams only: every tick.
    transport.every_tick = true;
    send(12);
    receiver.drain(base + (13 * FPP));
    size_t const xdp_before = transport.receives;
    for (uint64_t t = 1; t <= 8; ++t) {
        receiver.drain(base + (13 * FPP) + (t * 6));
    }
    EXPECT_EQ(transport.receives - xdp_before, size_t{8});
}

// The c= rule: a group and this host's address are received where they name; an offer (c= the origin, AES67-2023
// Clause 8.5.2) here, from that sender alone, its reports going back to it.
TEST(aes67_receiver, an_offer_is_received_here_from_its_sender_alone)
{
    LoopTransport transport;
    IPv4Address const sender{10, 0, 0, 5};
    auto session = stream_session({.address = sender, .port = 5010});
    session.origin_address = "10.0.0.5";
    EXPECT_TRUE(
        receive_endpoint(session, session.media[0], *session.connection, transport) ==
        (Endpoint{.address = transport.local_address(), .port = 5010}));
    Receiver receiver{STEREO_L24, {}, transport};
    EXPECT_TRUE(receiver.configure(session).has_value());
    Endpoint const here{.address = transport.local_address(), .port = 5010};
    transport.queue.push_back({here, rtp_packet(1, 952)});
    receiver.drain(952);
    EXPECT_EQ(receiver.counters().drops.source, uint64_t{1});  // from 192.168.1.10, not the sender
    transport.source.address = sender;                         // the datagrams' source from here
    transport.queue.push_back({here, rtp_packet(2, 952 + FPP)});
    receiver.drain(952 + FPP);
    EXPECT_EQ(receiver.counters().packets, uint64_t{1});
    auto const rtcp = receiver.rtcp_options();
    EXPECT_TRUE(rtcp.has_value());
    EXPECT_TRUE(rtcp->local == (Endpoint{.address = here.address, .port = 5011}));
    EXPECT_TRUE(rtcp->destination == (Endpoint{.address = sender, .port = 5011}));
}

TEST(aes67_receiver, a_transport_file_and_a_group_are_received_at_their_c_address)
{
    LoopTransport const transport;  // local 192.168.1.10
    IPv4Address const local{192, 168, 1, 10};
    auto own = stream_session({.address = local, .port = 5012});
    own.origin_address = "10.0.0.5";
    EXPECT_TRUE(receive_endpoint(own, own.media[0], *own.connection, transport) == (Endpoint{.address = local, .port = 5012}));
    auto group = stream_session({.address = IPv4Address{239, 69, 1, 9}, .port = 5014});
    group.origin_address = "239.69.1.9";  // never an offer
    EXPECT_TRUE(
        receive_endpoint(group, group.media[0], *group.connection, transport) ==
        (Endpoint{.address = IPv4Address{239, 69, 1, 9}, .port = 5014}));
    auto looped = stream_session({.address = local, .port = 5016});
    looped.origin_address = "192.168.1.10";  // our own stream, c= and o= ours
    EXPECT_TRUE(
        receive_endpoint(looped, looped.media[0], *looped.connection, transport) == (Endpoint{.address = local, .port = 5016}));
    // another host's, or no host at all: refused by the host's addresses, not by a bind the kernel may allow
    auto foreign = stream_session({.address = IPv4Address{10, 0, 0, 9}, .port = 5018});
    foreign.origin_address = "10.0.0.5";
    EXPECT_TRUE(
        receive_endpoint(foreign, foreign.media[0], *foreign.connection, transport).error() == Aes67Error::foreign_destination);
    auto none = stream_session({.address = IPv4Address{}, .port = 5020});
    none.origin_address = "0.0.0.0";
    EXPECT_TRUE(receive_endpoint(none, none.media[0], *none.connection, transport).error() == Aes67Error::foreign_destination);
}

TEST(aes67_receiver, a_source_specific_group_is_joined_for_its_source)
{
    LoopTransport transport;
    IPv4Address const sender{10, 0, 0, 5};
    auto session = stream_session({.address = IPv4Address{232, 10, 1, 1}, .port = 5004});
    session.source_filter = padl::sdp::SourceFilter{.destination = IPv4Address{232, 10, 1, 1}, .source = sender};
    Receiver receiver{STEREO_L24, {}, transport};
    EXPECT_TRUE(receiver.configure(session).has_value());
    EXPECT_EQ(transport.joined_source, std::optional{sender});
    auto const rtcp = receiver.rtcp_options(46);  // its RTCP joins for the same source: the report stays INCLUDE
    EXPECT_TRUE(rtcp && rtcp->source == std::optional{sender});
}

TEST(aes67_receiver, an_any_source_group_or_an_unfiltered_one_is_joined_for_any_source)
{
    LoopTransport transport;
    IPv4Address const sender{10, 0, 0, 5};
    auto session = stream_session({.address = IPv4Address{239, 10, 1, 1}, .port = 5004});
    session.source_filter = padl::sdp::SourceFilter{.destination = std::nullopt, .source = sender};
    Receiver receiver{STEREO_L24, {}, transport};
    EXPECT_TRUE(receiver.configure(session).has_value());
    EXPECT_FALSE(transport.joined_source.has_value());
    auto unfiltered = stream_session({.address = IPv4Address{232, 10, 1, 1}, .port = 5004});
    EXPECT_TRUE(receiver.configure(unfiltered).has_value());
    EXPECT_FALSE(transport.joined_source.has_value());
}

// A frame port that admits and places by the Depacketizer gives the frames, counters and report inputs a packet port
// gives the Receiver, packet for packet: reorder, loss, duplicates, strays, multiplexed RTCP, its ring wrapping.
TEST(aes67_receiver, a_frame_port_hands_on_and_counts_what_the_packet_path_does)
{
    TwoPaths paths;
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    uint32_t const offset = 0x1000;
    EXPECT_TRUE(paths.configure(stream_session(group, offset)));
    EXPECT_TRUE(paths.frames.frame_rx != nullptr && paths.packets.frame_rx == nullptr);
    uint64_t const base = (1ULL << 32U) + 96;  // the timestamps wrap
    auto at = [&](uint16_t const n) { return base + (uint64_t{n} * FPP); };
    for (uint16_t n : {0, 1, 2, 4, 3, 3, 6}) {  // 3 late, then again; 5 lost
        paths.push(group, offset_packet(static_cast<uint16_t>(10 + n), at(n), offset));
    }
    paths.push(group, offset_packet(17, at(7), offset, 0x9999));      // another source
    paths.push(group, offset_packet(17, at(7), offset, 0x1234, 96));  // another payload type
    paths.push(group, {0x80, 0x61});                                  // malformed
    paths.push(group, {0x80, 200, 0x00, 0x01});                       // multiplexed RTCP: not a refusal
    paths.drain(at(7));
    paths.expect_same();
    EXPECT_EQ(paths.frame.counters().packets, uint64_t{6});
    for (uint16_t n = 7; n < 40; ++n) {  // 1584 frames: the frame port's ring wraps
        paths.push(group, offset_packet(static_cast<uint16_t>(10 + n), at(n), offset));
        paths.drain(at(n + 1));  // when it was sent: the packet path's due-tick gate is open
        paths.expect_same();
    }
    // A new source, adopted after a second's silence.
    uint64_t const later = at(40) + 48'000;
    paths.push(group, offset_packet(500, later, offset, 0x5555));
    paths.drain(later);
    paths.expect_same();
    EXPECT_TRUE(paths.frame.ssrc() == std::optional<uint32_t>{0x5555});
    EXPECT_TRUE(paths.frame.state() == State::running);
}

// The offset reaches a frame port from the draining thread, and a sender-clocked stream's new source makes both paths
// forget it.
TEST(aes67_receiver, a_frame_port_follows_the_offset_as_the_packet_path_does)
{
    TwoPaths paths{true};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    auto session = stream_session(group, std::nullopt);
    session.media[0].media_clock = padl::sdp::MediaClock::sender();
    EXPECT_TRUE(paths.configure(session));
    auto& port = *paths.frames.frame_rx;
    EXPECT_TRUE(port.plan.sender_clock && !port.plan.offset.has_value());
    uint64_t const base = 48'000ULL * 5000;
    paths.push(group, rtp_packet(0, static_cast<uint32_t>(base)));
    paths.drain(base);
    paths.expect_same();
    EXPECT_EQ(paths.frame.counters().drops.no_offset, uint64_t{1});
    EXPECT_TRUE(port.offsets.empty());  // nothing changed: nothing passed on
    paths.packet.set_offset(0);
    paths.frame.set_offset(0);
    paths.push(group, rtp_packet(1, static_cast<uint32_t>(base + FPP)));
    paths.drain(base + FPP);
    paths.expect_same();
    EXPECT_TRUE(port.offsets == std::vector<std::optional<uint32_t>>{0U});
    EXPECT_EQ(paths.frame.counters().packets, uint64_t{1});
    // Another sender after a second's silence: both forget the offset until its reports give one.
    paths.push(group, rtp_packet(900, static_cast<uint32_t>(base + 48'000 + FPP), 0x7777));
    paths.drain(base + 48'000 + FPP);
    paths.expect_same();
    EXPECT_FALSE(paths.frame.offset().has_value());
    EXPECT_FALSE(port.offset.has_value());
    paths.frame.set_offset(0);
    paths.drain(base + 48'000 + (2 * FPP));
    EXPECT_TRUE(port.offset == std::optional<uint32_t>{0});
}

// The plan a transport is asked to open: the SDP's group and port, its source filter's source to join for and admit,
// its payload type, packet size and offset.
TEST(aes67_receiver, a_receive_plan_carries_what_the_sdp_says)
{
    LoopTransport transport;
    IPv4Address const sender{10, 0, 0, 5};
    auto session = stream_session({.address = IPv4Address{232, 10, 1, 1}, .port = 5004}, 0x1000);
    session.source_filter = padl::sdp::SourceFilter{.destination = IPv4Address{232, 10, 1, 1}, .source = sender};
    session.media[0].max_packet_time_us = 4000;
    Receiver receiver{STEREO_L24, {}, transport};
    EXPECT_TRUE(receiver.configure(session).has_value());
    EXPECT_EQ(transport.receive_plans.size(), size_t{1});
    ReceivePlan const expected{
        .destination = {.address = IPv4Address{232, 10, 1, 1}, .port = 5004},
        .join_source = sender,
        .format = STEREO_L24,
        .payload_type = 97,
        .max_frames = 192,
        .source_filter = sender,
        .offset = 0x1000,
        .sender_clock = false,
    };
    EXPECT_TRUE(transport.receive_plans.back() == expected);
    EXPECT_EQ(transport.joined_source, std::optional{sender});  // the default open_receive_stream(): joined for it
    // An offer: received here, admitted from its sender alone, joined for none.
    auto offer = stream_session({.address = sender, .port = 5010});
    offer.origin_address = "10.0.0.5";
    auto const plan = receive_plan(offer, offer.media[0], *offer.connection, STEREO_L24, false, transport);
    EXPECT_TRUE(plan && plan->destination == (Endpoint{.address = transport.local_address(), .port = 5010}));
    EXPECT_TRUE(plan && !plan->join_source && plan->source_filter == std::optional{sender});
}

// A receive plan names the grandmaster the stream's ts-refclk does (the media level's over the session's), none when it
// names none or only says traceable.
TEST(aes67_receiver, a_receive_plan_names_the_grandmaster_its_ts_refclk_does)
{
    LoopTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    auto session = stream_session({.address = IPv4Address{239, 69, 1, 2}, .port = 5004});
    EXPECT_TRUE(receiver.configure(session).has_value());
    EXPECT_FALSE(transport.receive_plans.back().grandmaster.has_value());
    session.clocks = padl::sdp::ReferenceClocks::of(padl::sdp::PtpReference{.traceable = true});
    EXPECT_TRUE(receiver.configure(session).has_value());
    EXPECT_FALSE(transport.receive_plans.back().grandmaster.has_value());
    Grandmaster const gptp{.identity = GRANDMASTER.identity, .version = padl::sdp::PtpVersion::ieee802_1as, .traceable = true};
    session.clocks = gptp.references();
    EXPECT_TRUE(receiver.configure(session).has_value());
    EXPECT_TRUE(transport.receive_plans.back().grandmaster == std::optional{gptp});
    Grandmaster const domain7{.identity = GRANDMASTER.identity, .domain = 7};
    session.media[0].clocks = domain7.references();
    EXPECT_TRUE(receiver.configure(session).has_value());
    EXPECT_TRUE(transport.receive_plans.back().grandmaster == std::optional{domain7});
}

// A drain that adopts a new source opens the due-tick gate, as a reset stream's is, even when it places nothing (a
// sender-clocked stream's new source, its offset forgotten): after a step back the port is asked at once.
TEST(aes67_receiver, a_new_source_reopens_the_due_tick_gate)
{
    LoopTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    receiver.set_sender_clock(true);
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    auto session = stream_session(group, std::nullopt);
    session.media[0].media_clock = padl::sdp::MediaClock::sender();
    EXPECT_TRUE(receiver.configure(session));
    receiver.set_offset(0);
    uint64_t const base = 48'000ULL * 5000;
    transport.queue.push_back({group, rtp_packet(0, static_cast<uint32_t>(base), 0xA)});
    receiver.drain(base + FPP);  // placed: the gate closes until a tick before base + 2 * FPP
    EXPECT_EQ(receiver.counters().packets, uint64_t{1});
    uint64_t const later = base + 96'000;
    transport.queue.push_back({group, rtp_packet(1, static_cast<uint32_t>(later), 0xB)});
    receiver.drain(later + FPP);  // another source after two seconds' silence: adopted, its offset forgotten
    EXPECT_FALSE(receiver.offset().has_value());
    EXPECT_FALSE(receiver.ssrc().has_value());  // learnt from its next packet
    size_t const asked = transport.receives;
    receiver.drain(base + 60);  // back inside the old gate's window
    EXPECT_TRUE(transport.receives > asked);
}

// A receiver without a FrameHandler drains a frame port as the packet path drains a socket: counted, handed nowhere.
TEST(aes67_receiver, a_frame_port_needs_no_handler)
{
    LoopTransport transport;
    transport.frame_ports = true;
    Receiver receiver{STEREO_L24, {}, transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    transport.queue.push_back({group, rtp_packet(1, 952)});
    receiver.drain(952 + FPP);
    EXPECT_EQ(receiver.counters().packets, uint64_t{1});
    EXPECT_TRUE(receiver.state() == State::running);
}

TEST_MAIN(padl_aes67, aes67_receiver_test)
