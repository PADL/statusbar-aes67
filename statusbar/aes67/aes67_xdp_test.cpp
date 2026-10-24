// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/aes67/aes67_xdp_transport.hpp"
#include "statusbar/rtp/rtp_frame.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <memory>
#include <print>
#include <vector>

using namespace statusbar;
using namespace statusbar::aes67;
using statusbar::ip::IPv4Address;

TEST(aes67_xdp, ships_a_umem_sized_for_the_rings_and_the_tx_pool)
{
    // The senders' pool is never offered to the kernel: the rest of the UMEM
    // must still cover a full fill ring with frames to spare for the RX ring
    // and frames held for receivers.
    auto const config = XdpTransport::config_for("eth0");
    EXPECT_EQ(config.fill_ring_size, XdpTransport::FILL_RING);
    EXPECT_EQ(config.umem_frame_count, XdpTransport::UMEM_FRAMES);
    EXPECT_TRUE(config.umem_frame_count >= config.fill_ring_size + XdpTransport::TX_POOL + XdpTransport::RECEIVE_BATCH);
    EXPECT_EQ(config.rx_drain_max, XdpTransport::RECEIVE_BATCH);
    EXPECT_TRUE(config.default_action == net::XdpAction::pass_kernel);
    EXPECT_TRUE(config.bind_mode == net::XdpBindMode::auto_);
    EXPECT_TRUE(config.attach_mode == net::XdpAttachMode::native);
    EXPECT_TRUE(config.pin_path.empty());
}

TEST(aes67_xdp, passes_the_attach_mode_and_pin_path_through)
{
    XdpTransport::Options const options{
        .bind_mode = net::XdpBindMode::copy,
        .attach_mode = net::XdpAttachMode::generic,
        .pin_path = net::XdpProgram::pin_path_for("eth0"),
    };
    auto const config = XdpTransport::config_for("eth0", options);
    EXPECT_TRUE(config.attach_mode == net::XdpAttachMode::generic);
    EXPECT_TRUE(config.pin_path == "/sys/fs/bpf/statusbar/eth0");
    EXPECT_TRUE(config.adopt);
}

TEST(aes67_xdp, refused_kicks_are_counted_and_fail_the_transport_after_a_millisecond)
{
    XdpTransport::TxHealth health;
    EXPECT_EQ(health.failures(), uint64_t{0});
    EXPECT_FALSE(health.error());
    EXPECT_FALSE(health.failing());
    for (uint32_t i = 1; i < XdpTransport::TX_FAILING_AFTER; ++i) {
        health.record(false, EINVAL);
        EXPECT_FALSE(health.failing());
    }
    health.record(true, 0);  // one kick through: the count in a row starts again
    for (uint32_t i = 1; i < XdpTransport::TX_FAILING_AFTER; ++i) {
        health.record(false, EINVAL);
    }
    EXPECT_FALSE(health.failing());
    health.record(false, EINVAL);
    EXPECT_TRUE(health.failing());
    EXPECT_EQ(health.failures(), uint64_t{(2 * XdpTransport::TX_FAILING_AFTER) - 1});
    EXPECT_TRUE(health.error() == std::errc::invalid_argument);
    health.record(true, 0);
    EXPECT_FALSE(health.failing());
    EXPECT_TRUE(health.error() == std::errc::invalid_argument);  // the last refusal is kept for the log
}

TEST(aes67_xdp, steers_rss_to_the_socket_queue_by_default)
{
    EXPECT_TRUE(XdpTransport::Options{}.steer_rss);
    EXPECT_EQ(XdpTransport::config_for("eth0").queue_id, XdpTransport::QUEUE);
}

TEST(aes67_xdp, passes_the_bind_mode_through)
{
    auto const copy = XdpTransport::config_for("eth0", {.bind_mode = net::XdpBindMode::copy});
    EXPECT_TRUE(copy.bind_mode == net::XdpBindMode::copy);
    auto const zero_copy = XdpTransport::config_for("eth0", {.bind_mode = net::XdpBindMode::zero_copy});
    EXPECT_TRUE(zero_copy.bind_mode == net::XdpBindMode::zero_copy);
    EXPECT_EQ(zero_copy.fill_ring_size, XdpTransport::FILL_RING);
}

TEST(aes67_xdp, a_frame_that_does_not_fit_goes_back_to_the_pool)
{
    std::array<uint8_t, 2048> big{};
    std::array<uint8_t, 40> small{};  // less than the 42 bytes of headers
    XdpTransport::TxPool pool;
    EXPECT_FALSE(XdpTransport::write_frame(pool, {}, {}, {}).has_value());  // empty pool
    EXPECT_EQ(pool.empty_count(), uint64_t{1});
    EXPECT_TRUE(pool.put({.handle = 1, .buffer = big}));
    EXPECT_TRUE(pool.put({.handle = 2, .buffer = small}));

    rtp::FrameAddressing const addressing{
        .destination_mac = rtp::multicast_mac(IPv4Address{239, 69, 1, 2}),
        .destination_address = IPv4Address{239, 69, 1, 2},
        .destination_port = 5004,
    };
    rtp::RtpHeader header{};
    header.init(96, 1, 2, 3);
    std::array<uint8_t, 288> const payload{};
    // The small frame is on top: it does not fit and goes back.
    EXPECT_FALSE(XdpTransport::write_frame(pool, addressing, header, payload).has_value());
    EXPECT_EQ(pool.size(), size_t{2});
    // Take it out of the way, and the big one is written.
    auto const spare = pool.take();
    EXPECT_EQ(spare->handle, uint64_t{2});
    auto const written = XdpTransport::write_frame(pool, addressing, header, payload);
    EXPECT_TRUE(written.has_value());
    EXPECT_EQ(written->first.handle, uint64_t{1});
    EXPECT_EQ(written->second, rtp::rtp_frame_overhead(false) + rtp::RtpHeader::LENGTH + payload.size());
    EXPECT_EQ(pool.size(), size_t{0});
    EXPECT_EQ(pool.empty_count(), uint64_t{1});  // only the take that found nothing counts
    EXPECT_TRUE(rtp::parse_rtp_frame(std::span{big}.first(written->second)).has_value());
}

TEST(aes67_xdp, renders_the_payload_into_the_pooled_frame)
{
    rtp::FrameAddressing const addressing{
        .destination_mac = rtp::multicast_mac(IPv4Address{239, 69, 1, 2}),
        .source_mac = ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x01},
        .source_address = IPv4Address{10, 0, 0, 1},
        .destination_address = IPv4Address{239, 69, 1, 2},
        .source_port = 5004,
        .destination_port = 5004,
        .ttl = 32,
    };
    // L16, L24 and L32 stereo, L16 and L24 8 channels, 48 frames: the bridge's AES67 payloads.
    for (size_t const length : {size_t{192}, size_t{288}, size_t{384}, size_t{768}, size_t{1152}}) {
        std::vector<uint8_t> payload(length);
        for (size_t i = 0; i < length; ++i) {
            payload[i] = static_cast<uint8_t>((i * 13) + length);
        }
        rtp::RtpHeader header{};
        header.init(97, static_cast<uint16_t>(length), 0x01020304U, 0xCAFEF00DU);

        std::array<uint8_t, 2048> copied{};
        std::array<uint8_t, 2048> rendered{};
        XdpTransport::TxPool pool;
        EXPECT_TRUE(pool.put({.handle = 1, .buffer = copied}));
        auto const written = XdpTransport::write_frame(pool, addressing, header, payload);
        EXPECT_TRUE(written.has_value());

        EXPECT_TRUE(pool.put({.handle = 2, .buffer = rendered}));
        uint8_t const* target = nullptr;
        auto const frame = XdpTransport::render_frame(pool, addressing, header, length, [&](std::span<uint8_t> const out) noexcept {
            target = out.data();
            std::ranges::copy(payload, out.begin());
            return true;
        });
        EXPECT_TRUE(frame.has_value());
        EXPECT_EQ(frame->first.handle, uint64_t{2});
        EXPECT_EQ(frame->second, written->second);
        // Straight into the UMEM frame, right after the 42 bytes of headers and the RTP header.
        EXPECT_TRUE(target == rendered.data() + rtp::rtp_frame_overhead(false) + rtp::RtpHeader::LENGTH);
        EXPECT_TRUE(std::ranges::equal(std::span{copied}.first(written->second), std::span{rendered}.first(frame->second)));

        std::array<uint8_t, 2048> reference{};
        auto const expected = rtp::serialize_rtp_frame(addressing, header, payload, reference);
        EXPECT_TRUE(expected.has_value());
        EXPECT_TRUE(std::ranges::equal(std::span{reference}.first(*expected), std::span{rendered}.first(frame->second)));
    }
}

TEST(aes67_xdp, an_abandoned_render_returns_the_frame_to_the_pool)
{
    std::array<uint8_t, 2048> big{};
    std::array<uint8_t, 40> small{};
    XdpTransport::TxPool pool;
    rtp::RtpHeader header{};
    header.init(96, 1, 2, 3);
    rtp::FrameAddressing const addressing{.destination_address = IPv4Address{239, 69, 1, 2}, .destination_port = 5004};

    size_t calls = 0;
    PayloadRenderer const count = [&calls](std::span<uint8_t>) noexcept {
        ++calls;
        return true;
    };
    // Empty pool: nothing to render into.
    EXPECT_FALSE(XdpTransport::render_frame(pool, addressing, header, 288, count).has_value());
    EXPECT_EQ(calls, size_t{0});
    // Too small for the headers: back to the pool, render not called.
    EXPECT_TRUE(pool.put({.handle = 1, .buffer = small}));
    EXPECT_FALSE(XdpTransport::render_frame(pool, addressing, header, 288, count).has_value());
    EXPECT_EQ(calls, size_t{0});
    EXPECT_EQ(pool.size(), size_t{1});
    (void)pool.take();
    // The render gives up: the frame goes back to the pool, not to the ring.
    EXPECT_TRUE(pool.put({.handle = 2, .buffer = big}));
    EXPECT_FALSE(XdpTransport::render_frame(pool, addressing, header, 288, [&calls](std::span<uint8_t>) noexcept {
                     ++calls;
                     return false;
                 }).has_value());
    EXPECT_EQ(calls, size_t{1});
    EXPECT_EQ(pool.size(), size_t{1});
    auto const back = pool.take();
    EXPECT_EQ(back->handle, uint64_t{2});
}

TEST(aes67_xdp, the_rtcp_pipe_copies_datagrams_and_drops_past_its_depth)
{
    XdpTransport::RtcpPipe pipe{Endpoint{.address = IPv4Address{239, 69, 1, 2}, .port = 5005}};
    std::array<uint8_t, 8> bytes{0x80, 0xC9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    Endpoint const source{.address = IPv4Address{10, 0, 0, 9}, .port = 5005};
    for (size_t i = 0; i < XdpTransport::RtcpPipe::DEPTH; ++i) {
        bytes[7] = static_cast<uint8_t>(i);
        EXPECT_TRUE(pipe.offer({.payload = bytes, .source = source}));
    }
    EXPECT_FALSE(pipe.offer({.payload = bytes, .source = source}));
    EXPECT_EQ(pipe.dropped(), uint64_t{1});
    bytes.fill(0);  // the pipe holds copies

    std::array<Datagram, 4> out{};
    std::array<XdpTransport::RtcpPipe::Frame, 4> storage{};
    size_t taken = 0;
    for (size_t n = pipe.take(out, storage); n > 0; n = pipe.take(out, storage)) {
        for (size_t i = 0; i < n; ++i) {
            EXPECT_EQ(out[i].payload.size(), size_t{8});
            EXPECT_EQ(out[i].payload[7], static_cast<uint8_t>(taken + i));
            EXPECT_TRUE(out[i].source == source);
        }
        taken += n;
    }
    EXPECT_EQ(taken, XdpTransport::RtcpPipe::DEPTH);
}

TEST(aes67_xdp, the_rtcp_pipe_drops_datagrams_longer_than_it_keeps)
{
    XdpTransport::RtcpPipe pipe{Endpoint{.address = IPv4Address{239, 69, 1, 2}, .port = 5005}};
    std::vector<uint8_t> const long_one(XdpTransport::RtcpPipe::MAX_DATAGRAM + 4, 0x80);
    EXPECT_FALSE(pipe.offer({.payload = long_one, .source = {}}));
    EXPECT_EQ(pipe.dropped(), uint64_t{1});
    std::array<Datagram, 1> out{};
    std::array<XdpTransport::RtcpPipe::Frame, 1> storage{};
    EXPECT_EQ(pipe.take(out, storage), size_t{0});
}

TEST(aes67_xdp, group_rules_pin_the_whole_address)
{
    auto const rule = XdpTransport::group_rule(IPv4Address{239, 69, 1, 2});
    // Destination MAC 01:00:5e:45:01:02, IPv4, IHL 5, UDP, 239.69 in bytes 30-31.
    EXPECT_EQ(rule.match[0], uint8_t{0x01});
    EXPECT_EQ(rule.match[3], uint8_t{0x45});
    EXPECT_EQ(rule.match[5], uint8_t{0x02});
    EXPECT_EQ(rule.match[12], uint8_t{0x08});
    EXPECT_EQ(rule.match[14], uint8_t{0x45});
    EXPECT_EQ(rule.match[23], uint8_t{17});
    EXPECT_EQ(rule.match[30], uint8_t{239});
    EXPECT_EQ(rule.match[31], uint8_t{69});
    EXPECT_EQ(rule.mask[30], uint8_t{0xFF});
    EXPECT_EQ(rule.mask[31], uint8_t{0xFF});
    // 239.197.1.2 shares the MAC (bit 7 of the second octet is not in it) but not the rule.
    auto const alias = XdpTransport::group_rule(IPv4Address{239, 197, 1, 2});
    EXPECT_TRUE(alias.mask == rule.mask);
    EXPECT_NE(alias.match[31], rule.match[31]);
    EXPECT_TRUE((rule.result_flags & net::flag::process) != 0);
}

// These need AF_XDP, libxdp and root; without them open() must fail cleanly
// and the tests end there.

TEST(aes67_xdp, open_fails_cleanly_or_serves_multicast_only)
{
    EXPECT_FALSE(XdpTransport::open("no-such-interface0").has_value());

    auto transport = XdpTransport::open("lo", {.bind_mode = net::XdpBindMode::copy});
    if (!transport) {
        std::println("  skipped: AF_XDP unavailable ({})", transport.error().message());
        return;
    }
    EXPECT_FALSE((*transport)->zero_copy());  // copy was forced (and lo has no zero-copy)
    EXPECT_TRUE((*transport)->bind_mode() == net::XdpBindMode::copy);
    EXPECT_TRUE((*transport)->drain_every_tick());
    auto const rss = (*transport)->rss_status();
    EXPECT_TRUE(rss && !*rss && rss->error() == std::errc::operation_not_supported);  // lo has no RSS
    Endpoint const unicast{.address = IPv4Address{127, 0, 0, 1}, .port = 5004};
    EXPECT_TRUE((*transport)->open_receive(unicast).error() == Aes67Error::unsupported_destination);
    EXPECT_TRUE((*transport)->open_send({.destination = unicast}).error() == Aes67Error::unsupported_destination);
    EXPECT_EQ((*transport)->socket_stats().tx_count, uint64_t{0});
    auto sender = (*transport)->open_send({.destination = {.address = IPv4Address{239, 69, 250, 3}, .port = 5004}});
    EXPECT_TRUE(sender.has_value());
    (*transport)->begin_tick();  // fills the TX pool
    rtp::RtpHeader header{};
    header.init(96, 1, 2, 3);
    std::array<uint8_t, 288> const payload{};
    EXPECT_TRUE((*sender)->send(header, payload));
    (*transport)->end_tick();
    EXPECT_EQ((*transport)->tx_flush_failures(), uint64_t{0});
    EXPECT_FALSE((*transport)->send_error());
    EXPECT_EQ((*transport)->socket_stats().tx_count, uint64_t{1});  // published by end_tick()
    EXPECT_EQ((*transport)->tx_pool_empty_count(), uint64_t{0});
}

TEST(aes67_xdp, joins_and_ticks_when_privileged)
{
    if (::geteuid() != 0) {
        std::println("  skipped: not root");
        return;
    }
    auto transport = XdpTransport::open("lo", {.steer_rss = false});
    if (!transport) {
        std::println("  skipped: AF_XDP unavailable ({})", transport.error().message());
        return;
    }
    EXPECT_FALSE((*transport)->rss_status().has_value());  // not asked for
    Endpoint const group{.address = IPv4Address{239, 69, 250, 2}, .port = 5004};
    auto port = (*transport)->open_receive(group);
    if (!port) {
        std::println("  skipped: cannot join on lo ({})", port.error().message());
        return;
    }
    std::array<Datagram, 4> out{};
    (*transport)->begin_tick();  // takes the port and installs its rule
    EXPECT_TRUE((*transport)->rules_status().has_value());
    EXPECT_EQ((*port)->receive(out), size_t{0});
    (*transport)->end_tick();
    // Past MAX_GROUPS groups is refused; closing ports is applied by the next tick.
    std::vector<std::unique_ptr<ReceivePort>> ports;
    for (uint8_t i = 0; i < XdpTransport::MAX_GROUPS + 1; ++i) {
        auto more = (*transport)->open_receive({.address = IPv4Address{239, 69, 251, i}, .port = 5004});
        if (!more) {
            EXPECT_TRUE(more.error() == net::XdpError::too_many_filter_rules);
            break;
        }
        ports.push_back(std::move(*more));
    }
    EXPECT_EQ(ports.size(), XdpTransport::MAX_GROUPS - 1);  // one group is already joined
    (*transport)->begin_tick();
    EXPECT_TRUE((*transport)->rules_status().has_value());
    ports.clear();
    port->reset();
    (*transport)->begin_tick();
    (*transport)->end_tick();
    EXPECT_TRUE((*transport)->rules_status().has_value());
}

TEST_MAIN(statusbar_aes67, aes67_xdp_test)
