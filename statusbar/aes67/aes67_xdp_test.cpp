// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/aes67/aes67_xdp_transport.hpp"
#include "statusbar/rtp/rtp_frame.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

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
    EXPECT_TRUE(rtp::parse_rtp_frame(std::span{big}.first(written->second)).has_value());
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
    Endpoint const unicast{.address = IPv4Address{127, 0, 0, 1}, .port = 5004};
    EXPECT_TRUE((*transport)->open_receive(unicast).error() == Aes67Error::unsupported_destination);
    EXPECT_TRUE((*transport)->open_send({.destination = unicast}).error() == Aes67Error::unsupported_destination);
    (*transport)->begin_tick();
    (*transport)->end_tick();
}

TEST(aes67_xdp, joins_and_ticks_when_privileged)
{
    if (::geteuid() != 0) {
        std::println("  skipped: not root");
        return;
    }
    auto transport = XdpTransport::open("lo");
    if (!transport) {
        std::println("  skipped: AF_XDP unavailable ({})", transport.error().message());
        return;
    }
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
