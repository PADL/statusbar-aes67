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

/// Every byte of `block` is the pattern for its index.
inline void expect_pattern(Received const& block)
{
    for (size_t i = 0; i < block.bytes.size(); ++i) {
        EXPECT_EQ(block.bytes[i], pattern(block.first_index + (i / 6), i % 6));
    }
}

}  // namespace

TEST(aes67_udp, two_receivers_share_a_port)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    uint16_t const port = 45030;
    Endpoint const unicast{.address = own_loopback(), .port = port};
    Endpoint const group{.address = own_group(), .port = port};
    RecordingSink unicast_sink;
    Receiver unicast_receiver{STEREO_L24, unicast_sink.handler(), **transport};
    EXPECT_TRUE(unicast_receiver.configure(stream_session(unicast)));
    RecordingSink group_sink;
    Receiver group_receiver{STEREO_L24, group_sink.handler(), **transport};
    bool const multicast = group_receiver.configure(stream_session(group)).has_value();

    // A second unicast receiver on the same address and port is refused.
    RecordingSink other_sink;
    Receiver other{STEREO_L24, other_sink.handler(), **transport};
    EXPECT_TRUE(other.configure(stream_session(unicast)).error() == std::errc::address_in_use);

    auto socket = net::make_udp_socket();
    EXPECT_TRUE(socket.has_value());
    EXPECT_TRUE(net::set_multicast_send(socket->get(), ::if_nametoindex("lo"), 1, true).has_value());
    auto send_to = [&](Endpoint const& to, uint16_t const seq) {
        auto const bytes = rtp_packet(seq, static_cast<uint32_t>(seq * FPP));
        sockaddr_in const address = net::ipv4_sockaddr_in(to.address, to.port);
        return ::sendto(socket->get(), bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr const*>(&address), sizeof(address));
    };
    for (uint16_t seq = 0; seq < 3; ++seq) {
        EXPECT_TRUE(send_to(unicast, seq) > 0);
        if (multicast) {
            (void)send_to(group, static_cast<uint16_t>(seq + 1000));
        }
    }
    uint64_t const now = 1000 * FPP;
    for (int tries = 0; tries < 100 && unicast_receiver.counters().packets < 3; ++tries) {
        unicast_receiver.drain(now);
        group_receiver.drain(now);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    unicast_receiver.drain(now);
    group_receiver.drain(now);
    EXPECT_EQ(unicast_receiver.counters().packets, uint64_t{3});
    EXPECT_EQ(unicast_receiver.counters().dropped, uint64_t{0});  // the group's datagrams never reach it
    if (!multicast || group_receiver.counters().packets == 0) {
        std::println("  skipped (multicast part): no multicast on lo here");
        return;
    }
    EXPECT_EQ(group_receiver.counters().packets, uint64_t{3});
    EXPECT_EQ(group_receiver.counters().dropped, uint64_t{0});  // nor the unicast ones it
}

TEST(aes67_udp, sender_to_receiver_over_loopback)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    EXPECT_TRUE((*transport)->local_address() == (IPv4Address{127, 0, 0, 1}));

    Endpoint const destination{.address = own_loopback(), .port = 45004};
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), **transport, "Test"};
    sender.set_send_offset(0);  // these tests time packets by their last frame
    EXPECT_TRUE(sender.configure(Destination{.address = destination.address, .port = destination.port, .payload_type = 97}));
    sender.set_grandmaster(GRANDMASTER);

    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), **transport};
    // The receiver configured from the sender's own SDP, as text and back.
    auto const sdp_text = padl::sdp::to_string(*sender.session());
    auto const parsed = padl::sdp::parse(sdp_text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(receiver.configure(*parsed));

    // A fake PTP clock: 1 ms (one packet) per step, from an index past 2^32.
    uint64_t now = (5ULL << 32U) + 47;
    for (int step = 0; step < 10; ++step) {
        sender.tick(now, 0);
        receiver.drain(now);
        for (int tries = 0; tries < 100 && receiver.counters().packets < static_cast<uint64_t>(step + 1); ++tries) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            receiver.drain(now);
        }
        now += FPP;
    }
    receiver.drain(now);
    EXPECT_EQ(receiver.counters().packets, uint64_t{10});
    EXPECT_EQ(sink.blocks.size(), size_t{10});
    // Packets lie on the 48-frame grid; the second one sent is the first kept.
    uint64_t const first = ((((5ULL << 32U) + 48) / FPP) * FPP) - FPP;  // the first packet sent
    for (size_t i = 0; i < sink.blocks.size(); ++i) {
        EXPECT_EQ(sink.blocks[i].first_index, first + (i * FPP));
        expect_pattern(sink.blocks[i]);
    }
    EXPECT_EQ(receiver.counters().lost, uint64_t{0});
    EXPECT_EQ(receiver.ssrc(), std::optional<uint32_t>{sender.ssrc()});
    EXPECT_EQ(sender.counters().packets, uint64_t{10});
}

TEST(aes67_udp, a_socket_sends_untagged_only)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const destination{.address = own_loopback(), .port = 45006};
    auto const tag = ieee::make_vlan_tag(2, false, 3);
    auto const tagged = (*transport)->open_send({.destination = destination, .vlan = tag});
    EXPECT_TRUE(!tagged && tagged.error() == std::errc::operation_not_supported);
    auto port = (*transport)->open_send({.destination = destination});
    EXPECT_TRUE(port.has_value());
    EXPECT_TRUE((*port)->set_vlan(std::nullopt));
    EXPECT_FALSE((*port)->set_vlan(tag));
}

TEST(aes67_udp, unicast_to_a_closed_port_is_not_refused)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), **transport, "Test"};
    sender.set_send_offset(0);
    // Nothing listens on this port: each datagram draws an ICMP port-unreachable.
    EXPECT_TRUE(sender.configure(Destination{.address = own_loopback(), .port = 45010}));
    uint64_t now = 48'000ULL * 900 + 47;
    for (int step = 0; step < 20; ++step, now += FPP) {
        sender.tick(now, 0);
        std::this_thread::sleep_for(std::chrono::microseconds{200});  // let the ICMP come back
    }
    EXPECT_EQ(sender.counters().dropped, uint64_t{0});
    EXPECT_EQ(sender.counters().packets, uint64_t{20});

    // A listener that appears later gets what follows.
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), **transport};
    auto session = stream_session({.address = own_loopback(), .port = 45010});
    session.media[0].payload_type = 96;
    EXPECT_TRUE(receiver.configure(session));
    for (int step = 0; step < 3; ++step, now += FPP) {
        sender.tick(now, 0);
    }
    for (int tries = 0; tries < 100 && receiver.counters().packets < 3; ++tries) {
        receiver.drain(now);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(receiver.counters().packets, uint64_t{3});
    EXPECT_EQ(sender.counters().dropped, uint64_t{0});
}

TEST(aes67_udp, counts_loss_and_reorder_from_the_wire)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const destination{.address = own_loopback(), .port = 45006};
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), **transport};
    EXPECT_TRUE(receiver.configure(stream_session(destination)));

    auto socket = net::make_udp_socket();
    EXPECT_TRUE(socket.has_value());
    sockaddr_in const to = net::ipv4_sockaddr_in(destination.address, destination.port);
    uint64_t const base = 48000ULL * 3600;
    for (uint16_t const n : {0, 1, 2, 4, 3, 6, 7}) {
        auto const bytes = rtp_packet(static_cast<uint16_t>(100 + n), static_cast<uint32_t>(base + (n * FPP)));
        EXPECT_EQ(
            ::sendto(socket->get(), bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr const*>(&to), sizeof(to)),
            static_cast<ssize_t>(bytes.size()));
    }
    uint64_t const now = base + (8 * FPP);
    for (int tries = 0; tries < 100 && receiver.counters().packets < 7; ++tries) {
        receiver.drain(now);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(receiver.counters().packets, uint64_t{7});
    EXPECT_EQ(receiver.counters().late, uint64_t{FPP});  // 3 after 4
    EXPECT_EQ(receiver.counters().lost, uint64_t{FPP});  // 5
    EXPECT_EQ(sink.blocks.back().first_index, base + (7 * FPP));
    expect_pattern(sink.blocks.back());
}

TEST(aes67_udp, a_truncated_datagram_is_counted_malformed)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const unicast{.address = own_loopback(), .port = 45012};
    auto port = (*transport)->open_receive(unicast);
    EXPECT_TRUE(port.has_value());
    auto socket = net::make_udp_socket();
    EXPECT_TRUE(socket.has_value());
    std::vector<uint8_t> const oversized(UdpTransport::MAX_DATAGRAM + 100, uint8_t{0x80});
    sockaddr_in const address = net::ipv4_sockaddr_in(unicast.address, unicast.port);
    EXPECT_TRUE(
        ::sendto(
            socket->get(), oversized.data(), oversized.size(), 0, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) >
        0);
    std::array<Datagram, 4> out{};
    for (int tries = 0; tries < 200 && (*transport)->receive_refusals().malformed == 0; ++tries) {
        EXPECT_EQ((*port)->receive(out), size_t{0});
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ((*transport)->receive_refusals().malformed, uint64_t{1});
    EXPECT_EQ((*transport)->receive_refusals().unmatched, uint64_t{0});
    (*transport)->reset_counters();
    EXPECT_EQ((*transport)->receive_refusals(), ReceiveRefusals{});
    port->reset();
}

TEST(aes67_udp, multicast_over_loopback_when_the_host_allows)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const group{.address = own_group(), .port = 45008};
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), **transport};
    if (!receiver.configure(stream_session(group))) {
        std::println("  skipped: cannot join a group on lo");
        return;
    }
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), **transport, "Test"};
    sender.set_send_offset(0);  // these tests time packets by their last frame
    EXPECT_TRUE(sender.configure(Destination{.address = group.address, .port = group.port, .payload_type = 97}));
    uint64_t now = 48000ULL * 7200 + 47;
    for (int step = 0; step < 5; ++step, now += FPP) {
        sender.tick(now, 0);
    }
    for (int tries = 0; tries < 50 && receiver.counters().packets < 5; ++tries) {
        receiver.drain(now);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (sender.counters().dropped != 0 || receiver.counters().packets == 0) {
        std::println("  skipped: the host does not route multicast on lo");
        return;
    }
    EXPECT_EQ(receiver.counters().packets, uint64_t{5});
    expect_pattern(sink.blocks.back());
}

namespace {

/// Wait for `count` datagrams on `port`, appending copies to `out`.
void receive_datagrams(ReceivePort& port, size_t const count, std::vector<std::vector<uint8_t>>& out)
{
    std::array<Datagram, UdpTransport::RECEIVE_BATCH> batch{};
    for (int tries = 0; tries < 200 && out.size() < count; ++tries) {
        size_t const n = port.receive(batch);
        for (size_t i = 0; i < n; ++i) {
            out.emplace_back(batch[i].payload.begin(), batch[i].payload.end());
        }
        if (out.size() < count) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
}

/// AES67's linear encodings (L16, L24 and L32), at a few channel counts.
constexpr std::array<StreamFormat, 6> RENDER_FORMATS{{
    {.encoding = padl::sdp::Encoding::l16, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = padl::sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = padl::sdp::Encoding::l16, .channels = 8, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = padl::sdp::Encoding::l24, .channels = 8, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = padl::sdp::Encoding::l32, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = padl::sdp::Encoding::l32, .channels = 6, .sample_rate = 48000, .frames_per_packet = FPP},  // 8 would not fit
}};

}  // namespace

namespace {

/// Wait for a datagram on `port`; its payload copied, or empty after 200 ms.
auto receive_rtcp(RtcpPort& port) -> std::pair<std::vector<uint8_t>, Endpoint>
{
    std::array<Datagram, 4> batch{};
    for (int tries = 0; tries < 200; ++tries) {
        if (port.receive(batch) > 0) {
            return {{batch[0].payload.begin(), batch[0].payload.end()}, batch[0].source};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return {};
}

}  // namespace

TEST(aes67_udp, an_rtcp_port_sends_to_and_receives_from_its_group)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    RtcpOptions const options{.destination = {.address = own_group(), .port = 45101}, .ttl = 1, .dscp = 34};
    auto a = (*transport)->open_rtcp(options);
    auto b = (*transport)->open_rtcp(options);
    if (!a || !b) {
        std::println("  skipped: cannot join a group on lo");
        return;
    }
    EXPECT_TRUE((*a)->fd() >= 0);
    int tos = 0;
    socklen_t length = sizeof(tos);
    EXPECT_EQ(::getsockopt((*a)->fd(), IPPROTO_IP, IP_TOS, &tos, &length), 0);
    EXPECT_EQ(tos, 34 << 2);  // AES67-2023 Clause 6.2: the stream's DSCP

    std::array<uint8_t, 8> const packet{0x80, 0xC9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07};
    EXPECT_TRUE((*a)->send(packet).has_value());
    auto const [bytes, source] = receive_rtcp(**b);
    if (bytes.empty()) {
        std::println("  skipped: the host does not route multicast on lo");
        return;
    }
    EXPECT_EQ(bytes, (std::vector<uint8_t>{packet.begin(), packet.end()}));
    EXPECT_EQ(source.port, uint16_t{45101});
    EXPECT_EQ((*b)->dropped(), uint64_t{0});
}

TEST(aes67_udp, a_unicast_rtcp_port_reaches_its_peer)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    IPv4Address const x = own_loopback();
    IPv4Address const y{127, static_cast<uint8_t>(x.octet(1) + 64), x.octet(2), x.octet(3)};
    uint16_t const port = 45111;
    auto a = (*transport)->open_rtcp({.destination = {.address = y, .port = port}, .local = {.address = x, .port = port}});
    auto b = (*transport)->open_rtcp({.destination = {.address = x, .port = port}, .local = {.address = y, .port = port}});
    EXPECT_TRUE(a.has_value());
    EXPECT_TRUE(b.has_value());
    // A unicast address and port is one port's: a second is refused rather than splitting its reports.
    auto again = (*transport)->open_rtcp({.destination = {.address = y, .port = port}, .local = {.address = x, .port = port}});
    EXPECT_TRUE(again.error() == std::errc::address_in_use);
    std::array<uint8_t, 8> const packet{0x80, 0xC9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x09};
    EXPECT_TRUE((*a)->send(packet).has_value());
    auto const [to_b, from_a] = receive_rtcp(**b);
    EXPECT_EQ(to_b.size(), packet.size());
    EXPECT_TRUE(from_a == (Endpoint{.address = x, .port = port}));
    EXPECT_TRUE((*b)->send(packet).has_value());
    auto const [to_a, from_b] = receive_rtcp(**a);
    EXPECT_EQ(to_a.size(), packet.size());
    EXPECT_TRUE(from_b == (Endpoint{.address = y, .port = port}));
}

// ts-refclk:localmac names the interface's hardware address; loopback's is all zeros, so none (RFC 7273 Section 4.8)
TEST(aes67_udp, the_loopback_interface_has_no_mac_to_name)
{
    auto const lo = find_interface("lo");
    EXPECT_TRUE(lo.has_value());
    EXPECT_FALSE(lo && lo->mac.has_value());
    auto const transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    EXPECT_FALSE(transport && (*transport)->local_mac().has_value());
}

TEST(aes67_udp, multicast_receive_sockets_receive_only_their_own_joins)
{
    auto const lo = find_interface("lo");
    EXPECT_TRUE(lo.has_value());
    auto const multicast_all = [](int const fd) {
        int all = -1;
        socklen_t length = sizeof(all);
        EXPECT_EQ(::getsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &all, &length), 0);
        return all;
    };
    auto const unicast = open_receive_socket(*lo, {.address = own_loopback(), .port = 45040}, true);
    EXPECT_TRUE(unicast.has_value());
    EXPECT_EQ(multicast_all(unicast->get()), 1);  // the kernel's default: nothing joined to filter by
    auto const group = open_receive_socket(*lo, {.address = own_group(), .port = 45040}, true);
    auto transport = UdpTransport::open("lo");
    auto rtcp = (*transport)->open_rtcp({.destination = {.address = own_group(), .port = 45041}});
    if (!group || !rtcp) {
        std::println("  skipped: cannot join a group on lo");
        return;
    }
    EXPECT_EQ(multicast_all(group->get()), 0);
    EXPECT_EQ(multicast_all((*rtcp)->fd()), 0);
}

TEST(aes67_udp, a_rendered_send_is_the_serialised_packet_byte_for_byte)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const destination{.address = own_loopback(), .port = 45020};
    auto rx = (*transport)->open_receive(destination);
    EXPECT_TRUE(rx.has_value());
    auto tx = (*transport)->open_send({.destination = destination});
    EXPECT_TRUE(tx.has_value());
    auto& port = **tx;

    uint16_t seq = 100;
    for (auto const& format : RENDER_FORMATS) {
        std::vector<uint8_t> payload(format.bytes_per_packet());
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = pattern(seq, i);
        }
        padl::rtp::RtpHeader header{};
        header.init(97, seq, 0x12345678U + seq, 0xCAFEF00DU);
        std::vector<uint8_t> expected(padl::rtp::RtpHeader::LENGTH + payload.size());
        EXPECT_TRUE(padl::rtp::serialize_rtp(header, payload, expected).has_value());

        size_t rendered_length = 0;
        EXPECT_TRUE(port.send_rendered(header, payload.size(), [&](std::span<uint8_t> const out) noexcept {
            rendered_length = out.size();
            std::ranges::copy(payload, out.begin());
            return true;
        }));
        EXPECT_EQ(rendered_length, payload.size());
        // An abandoned render uses no batch slot and sends nothing.
        EXPECT_FALSE(port.send_rendered(header, payload.size(), [](std::span<uint8_t> const out) noexcept {
            std::ranges::fill(out, uint8_t{0xEE});
            return false;
        }));
        EXPECT_EQ(port.flush(), size_t{0});

        std::vector<std::vector<uint8_t>> got;
        receive_datagrams(**rx, 2, got);
        EXPECT_TRUE(got == std::vector<std::vector<uint8_t>>{expected});
        ++seq;
    }

    // A full batch: send_rendered() refuses without calling render, and the
    // flush still sends the whole batch, per tick as before.
    std::array<uint8_t, 12> const small{};
    padl::rtp::RtpHeader header{};
    header.init(97, 1, 2, 3);
    for (size_t i = 0; i < UdpTransport::SEND_BATCH; ++i) {
        EXPECT_TRUE(port.send_rendered(header, small.size(), [](std::span<uint8_t> const out) noexcept {
            std::ranges::fill(out, uint8_t{1});
            return true;
        }));
    }
    bool called = false;
    EXPECT_FALSE(port.send_rendered(header, small.size(), [&](std::span<uint8_t>) noexcept {
        called = true;
        return true;
    }));
    EXPECT_FALSE(called);
    EXPECT_EQ(port.flush(), size_t{0});
    std::vector<std::vector<uint8_t>> got;
    receive_datagrams(**rx, UdpTransport::SEND_BATCH, got);
    EXPECT_EQ(got.size(), UdpTransport::SEND_BATCH);
    // Too large for a datagram: refused, render not called.
    EXPECT_FALSE(port.send_rendered(header, UdpTransport::MAX_DATAGRAM, [&](std::span<uint8_t>) noexcept {
        called = true;
        return true;
    }));
    EXPECT_FALSE(called);
    EXPECT_EQ(port.flush(), size_t{0});
}

TEST(aes67_udp, sender_renders_each_payload_once_straight_into_the_batch)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());

    uint16_t port_number = 45030;
    for (auto const& format : RENDER_FORMATS) {
        Endpoint const destination{.address = own_loopback(), .port = port_number++};
        auto rx = (*transport)->open_receive(destination);
        EXPECT_TRUE(rx.has_value());

        // Counts each pull and where it wrote: with a staging copy every pull
        // would land in the same buffer; rendered in place, each packet of a
        // tick lands in its own batch slot, right after its RTP header.
        uint32_t const frame_bytes = format.bytes_per_frame();
        PatternSource source{frame_bytes};
        std::vector<uint8_t const*> targets;
        size_t bytes_written = 0;
        FrameProvider provider = [&](uint64_t const first_index, uint32_t const frames, std::span<uint8_t> const out) {
            targets.push_back(out.data());
            EXPECT_EQ(out.size(), size_t{frames} * frame_bytes);
            if (!source.pull(first_index, frames, out)) {
                std::ranges::fill(out, uint8_t{0x55});  // a partial pull: the sender must still send silence
                return Supply::missing;
            }
            bytes_written += out.size();
            return Supply::frames;
        };
        Sender sender{format, std::move(provider), **transport, "Test"};
        sender.set_send_offset(0);
        EXPECT_TRUE(sender.configure(Destination{.address = destination.address, .port = destination.port, .payload_type = 98}));

        uint64_t const base = 48'000ULL * 2000;
        source.refuse_index = base + 48;  // the second packet of the burst is missing
        sender.tick(base - 1, 0);         // one packet: [base - 48, base)
        sender.tick(base + 143, 0);       // a burst of three in one tick
        EXPECT_EQ(source.pulls, size_t{4});
        EXPECT_EQ(targets.size(), size_t{4});
        for (size_t i = 2; i < targets.size(); ++i) {
            EXPECT_EQ(targets[i] - targets[i - 1], static_cast<std::ptrdiff_t>(UdpTransport::MAX_DATAGRAM));
        }
        EXPECT_EQ(bytes_written, size_t{3} * format.bytes_per_packet());  // one copy per packet, nothing more
        EXPECT_EQ(sender.counters().packets, uint64_t{4});
        EXPECT_EQ(sender.counters().lost, uint64_t{FPP});
        EXPECT_EQ(sender.counters().dropped, uint64_t{0});

        // Each datagram as the copy-in path built it: the provider's frames
        // (silence for the missing packet) behind the same header.
        std::vector<std::vector<uint8_t>> got;
        receive_datagrams(**rx, 4, got);
        EXPECT_EQ(got.size(), size_t{4});
        for (size_t i = 0; i < got.size(); ++i) {
            auto const packet = padl::rtp::parse_rtp(got[i]);
            EXPECT_TRUE(packet.has_value());
            uint64_t const first = base - 48 + (i * 48);
            EXPECT_EQ(packet->header.timestamp.get(), static_cast<uint32_t>(first));
            std::vector<uint8_t> payload(format.bytes_per_packet(), 0);
            if (first != source.refuse_index) {
                (void)source.pull(first, FPP, payload);
            }
            std::vector<uint8_t> expected(padl::rtp::RtpHeader::LENGTH + payload.size());
            EXPECT_TRUE(padl::rtp::serialize_rtp(packet->header, payload, expected).has_value());
            EXPECT_TRUE(got[i] == expected);
        }
    }
}

// A unicast c= that is neither ours nor the origin names another host: the kernel will not bind it.
TEST(aes67_udp, a_unicast_stream_to_another_host_is_refused)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Receiver receiver{STEREO_L24, {}, **transport};
    auto const refused = receiver.configure(stream_session({.address = IPv4Address{192, 0, 2, 77}, .port = 45020}));
    EXPECT_TRUE(!refused && refused.error() == Aes67Error::foreign_destination);
    EXPECT_TRUE(receiver.state() == State::error && receiver.error() == Aes67Error::foreign_destination);
}

// A unicast sender's socket leaves by its interface from its address, with the TTL asked and priority 0, so its
// DSCP never maps it to an SR class's queue.
TEST(aes67_udp, a_unicast_send_socket_is_pinned_to_its_interface_at_priority_zero)
{
    auto info = find_interface("lo");
    EXPECT_TRUE(info.has_value());
    if (!info) {
        return;
    }
    EXPECT_TRUE(info->loopback && is_local(*info, own_loopback()) && !is_local(*info, IPv4Address{10, 0, 0, 9}));
    auto fd = open_send_socket(*info, {.destination = {.address = own_loopback(), .port = 45022}, .ttl = 7});
    EXPECT_TRUE(fd.has_value());
    if (!fd) {
        return;
    }
    int value = -1;
    socklen_t length = sizeof(value);
    EXPECT_EQ(::getsockopt(fd->get(), SOL_SOCKET, SO_PRIORITY, &value, &length), 0);
    EXPECT_EQ(value, 0);
    EXPECT_EQ(::getsockopt(fd->get(), IPPROTO_IP, IP_TTL, &value, &length), 0);
    EXPECT_EQ(value, 7);
    sockaddr_in bound{};
    socklen_t bound_length = sizeof(bound);
    EXPECT_EQ(::getsockname(fd->get(), reinterpret_cast<sockaddr*>(&bound), &bound_length), 0);
    EXPECT_TRUE(net::ipv4_endpoint(bound).address == info->address);
    auto group = open_send_socket(*info, {.destination = {.address = own_group(), .port = 45022}});
    EXPECT_TRUE(group.has_value());
    EXPECT_EQ(::getsockopt(group->get(), SOL_SOCKET, SO_PRIORITY, &value, &length), 0);
    EXPECT_EQ(value, 2);  // a group's keeps the one AF41 maps to (TC_PRIO_BULK, rt_tos2priority())
    // a unicast stream's RTCP socket leaves as its stream's: by the interface, with its TTL, at priority 0
    auto rtcp = open_rtcp_socket(*info, {.destination = {.address = own_loopback(), .port = 45023}, .ttl = 9});
    EXPECT_TRUE(rtcp.has_value());
    if (!rtcp) {
        return;
    }
    EXPECT_EQ(::getsockopt((*rtcp)->fd(), SOL_SOCKET, SO_PRIORITY, &value, &length), 0);
    EXPECT_EQ(value, 0);
    EXPECT_EQ(::getsockopt((*rtcp)->fd(), IPPROTO_IP, IP_TTL, &value, &length), 0);
    EXPECT_EQ(value, 9);
    uint32_t index = 0;
    socklen_t index_length = sizeof(index);
    EXPECT_EQ(::getsockopt((*rtcp)->fd(), IPPROTO_IP, IP_UNICAST_IF, &index, &index_length), 0);
    EXPECT_EQ(ntohl(index), info->index);
}

TEST(aes67_udp, a_source_specific_group_is_joined_on_loopback)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    auto const own = own_group();
    Endpoint const group{.address = IPv4Address{232, own.octet(1), own.octet(2), own.octet(3)}, .port = 45232};
    auto port = (*transport)->open_receive_from(group, IPv4Address{127, 0, 0, 1});
    if (!port) {
        std::println("  skipped: cannot join a group on lo");
        return;
    }
    EXPECT_FALSE((*transport)->open_receive_from(group, IPv4Address{239, 1, 1, 1}).has_value());
}

TEST_MAIN(padl_aes67, aes67_udp_test)
