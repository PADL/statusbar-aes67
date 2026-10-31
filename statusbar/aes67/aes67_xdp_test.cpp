// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/aes67/aes67_xdp_transport.hpp"
#include "statusbar/net/net_tcam_classifier.hpp"
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

TEST(aes67_xdp, a_whole_frame_fits_a_receive_buffer_sized_in_kib)
{
    // igb writes RX buffers in 1 KiB units below the UMEM frame less the kernel's
    // 256-octet XDP headroom; a larger frame arrives cut to its first buffer.
    constexpr size_t XDP_HEADROOM = 256;
    constexpr size_t LARGEST_FRAME = 1522;  // 1500-octet MTU, VLAN tagged, FCS
    auto const config = XdpTransport::config_for("eth0");
    size_t const buffer = config.umem_frame_size - config.umem_headroom - XDP_HEADROOM;
    EXPECT_TRUE((buffer / 1024 * 1024) >= LARGEST_FRAME);
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
    health.reset_failures();
    EXPECT_EQ(health.failures(), uint64_t{0});
    EXPECT_TRUE(health.error() == std::errc::invalid_argument);  // the count, not the history
}

// Until a lead is in force every frame goes at now, in order; then each at its departure plus the
// lead, never before the last time given (igc's launch-time queue is a FIFO); an untimed frame at now.
TEST(aes67_xdp, launch_times_are_now_until_the_lead_is_in_force_then_departure_plus_lead)
{
    constexpr uint64_t T = 1'800'000'000ULL * 1'000'000'000ULL;
    XdpTransport::LaunchClock clock;
    EXPECT_FALSE(clock.lead_ns().has_value());
    EXPECT_EQ(clock.next(T, [] { return T - 5'000'000; }), T - 5'000'000);  // a departure, but no lead yet: now
    clock.set_lead_ns(125'000);
    EXPECT_EQ(clock.lead_ns(), std::optional<uint64_t>{125'000});
    EXPECT_EQ(clock.next(T, [] { return uint64_t{0}; }), T + 125'000);
    EXPECT_EQ(clock.next(T - 50'000, [] { return uint64_t{0}; }), T + 125'000);   // a second sender's, just behind: held
    EXPECT_EQ(clock.order().clamped(), uint64_t{0});                              // under a packet: not counted
    EXPECT_EQ(clock.next(T - 200'000, [] { return uint64_t{0}; }), T + 125'000);  // over a packet: counted
    EXPECT_EQ(clock.next(0, [] { return T + 200'000; }), T + 200'000);            // untimed: now
    EXPECT_EQ(clock.next(T - 3'000'000'000ULL, [] { return uint64_t{0}; }), T - 3'000'000'000ULL + 125'000);  // a step
    EXPECT_EQ(clock.order().clamped(), uint64_t{1});
    EXPECT_EQ(clock.order().max_clamp_ns(), uint64_t{200'000});
    EXPECT_EQ(clock.order().steps(), uint64_t{1});
    clock.set_lead_ns(0);
    EXPECT_EQ(clock.next(T + 1'000'000, [] { return uint64_t{0}; }), T + 1'000'000);
    clock.clear_lead();  // the clock no longer agrees: now again
    EXPECT_FALSE(clock.lead_ns().has_value());
    EXPECT_EQ(clock.next(T + 2'000'000, [] { return T + 1'500'000; }), T + 1'500'000);
}

// A kernel refusing the UMEM's TX metadata is probed once before the open: launch time goes off with the
// reason and the socket opens untimed; a probe that passes keeps it; not asked for, not probed.
TEST(aes67_xdp, a_kernel_refusing_tx_metadata_is_probed_before_the_open)
{
    int probes = 0;
    auto const refuses = [&probes] {
        ++probes;
        return Status{failure(net::XdpError::tx_metadata_not_supported)};
    };
    auto const passes = [&probes] {
        ++probes;
        return Status{};
    };
    auto const transport = XdpTransport::unopened();
    XdpTransport::Options const wanted{.launch_time = true};
    auto const refused = transport->with_launch_time(wanted, refuses);
    EXPECT_TRUE(!refused.launch_time && transport->launch_time_error() == net::XdpError::tx_metadata_not_supported && probes == 1);
    auto const kept = transport->with_launch_time(wanted, passes);
    EXPECT_TRUE(kept.launch_time && probes == 2);
    auto const unasked = transport->with_launch_time(XdpTransport::Options{}, refuses);
    EXPECT_TRUE(!unasked.launch_time && probes == 2);
    EXPECT_FALSE(transport->launch_time());
}

// Launch time asks the socket for TX metadata; the queue the etf goes on is the socket's, under our
// mqprio handle, with the interface's TX queues.
TEST(aes67_xdp, launch_time_asks_for_tx_metadata_on_the_socket_s_queue)
{
    EXPECT_FALSE(XdpTransport::config_for("eth0").tx_launch_time);
    EXPECT_TRUE(XdpTransport::config_for("eth0", {.launch_time = true}).tx_launch_time);
    auto const queue = XdpTransport::launch_time_queue(4);
    EXPECT_EQ(queue.major, XdpTransport::LAUNCH_TIME_QDISC_MAJOR);
    EXPECT_EQ(queue.queue, static_cast<uint16_t>(XdpTransport::QUEUE));
    EXPECT_EQ(queue.tx_queues, uint16_t{4});
    EXPECT_EQ(queue.etf_delta_ns, net::tc::DEFAULT_ETF_DELTA_NS);
    EXPECT_EQ(net::tc::launch_time_class(queue), net::tc::make_handle(XdpTransport::LAUNCH_TIME_QDISC_MAJOR, 1));
    // PTP on queue 2 of 4, the root kept without launch time; with 3 queues PTP shares the rest.
    EXPECT_TRUE(queue.ptp_queue == XdpTransport::PTP_QUEUE && queue.persistent_root);
    EXPECT_FALSE(XdpTransport::launch_time_queue(3).ptp_queue.has_value());
    EXPECT_TRUE(net::tc::launch_time_mqprio(queue).has_value() && net::tc::launch_time_mqprio(XdpTransport::launch_time_queue(3)));
    // Unsupported asks nothing of the kernel and looks nothing up: no interface, no privilege needed.
    auto const none = XdpTransport::settle_launch_time("no-such-interface-0", net::tc::LaunchTimeMode::unsupported);
    EXPECT_TRUE(none && *none == net::tc::LaunchTimeQueueAction::none);
    EXPECT_FALSE(XdpTransport::settle_launch_time("no-such-interface-0", net::tc::LaunchTimeMode::off).has_value());
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

namespace {

/// An RTP frame to `destination` from 10.0.0.2 with a 288-octet payload, untagged unless `vlan`.
auto rtp_frame_to(Endpoint const& destination, std::optional<ieee::VlanTag> const vlan = std::nullopt) -> std::vector<uint8_t>
{
    rtp::FrameAddressing const addressing{
        .destination_mac = rtp::multicast_mac(destination.address),
        .source_mac = {},
        .vlan = vlan,
        .source_address = IPv4Address{10, 0, 0, 2},
        .destination_address = destination.address,
        .source_port = destination.port,
        .destination_port = destination.port,
    };
    rtp::RtpHeader header{};
    header.init(97, 1, 2, 3);
    std::array<uint8_t, 288> const payload{};
    std::vector<uint8_t> frame(1514);
    auto const length = rtp::serialize_rtp_frame(addressing, header, payload, frame);
    frame.resize(length.value_or(0));
    return frame;
}

}  // namespace

TEST(aes67_xdp, joined_groups_are_redirected_untagged_and_every_group_tagged_with_the_vid)
{
    IPv4Address const group{239, 69, 1, 2};
    std::array const rules{XdpTransport::group_rule(group), XdpTransport::tagged_groups_rule(2)};
    net::TcamClassifier<32> classifier;
    classifier.set_rules(rules);
    auto const redirected = [&classifier](std::vector<uint8_t> const& frame) {
        return (classifier.classify_first(frame) & net::flag::process) != 0;
    };
    auto const tag = ieee::make_vlan_tag(2, false, 3);
    Endpoint const stream{.address = group, .port = 5004};
    Endpoint const other{.address = IPv4Address{239, 197, 7, 9}, .port = 5004};
    EXPECT_TRUE(redirected(rtp_frame_to(stream)));
    EXPECT_FALSE(redirected(rtp_frame_to(other)));
    EXPECT_TRUE(redirected(rtp_frame_to(stream, tag)));
    EXPECT_TRUE(redirected(rtp_frame_to(other, tag)));                                // the ports count it unmatched
    EXPECT_TRUE(redirected(rtp_frame_to(stream, ieee::make_vlan_tag(2, false, 0))));  // any PCP
    EXPECT_FALSE(redirected(rtp_frame_to(stream, ieee::make_vlan_tag(3, false, 3))));
    auto unicast = rtp_frame_to(stream, tag);
    unicast[0] = 0x02;
    EXPECT_FALSE(redirected(unicast));
    auto past_ipv4_range = rtp_frame_to(stream, tag);
    past_ipv4_range[3] |= 0x80U;  // 01:00:5e:80:...: not an IPv4 group's MAC
    EXPECT_FALSE(redirected(past_ipv4_range));
}

TEST(aes67_xdp, the_tagged_rule_goes_last_so_a_vid_change_leaves_the_group_slots_alone)
{
    auto const same = [](net::CompiledRule<32> const& a, net::CompiledRule<32> const& b) {
        return a.mask == b.mask && a.match == b.match && a.result_flags == b.result_flags;
    };
    std::array<XdpTransport::PortState, 3> states{{
        {{.address = IPv4Address{239, 69, 1, 1}, .port = 5004}, net::FileDescriptor{}},
        {{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}, net::FileDescriptor{}},
        {{.address = IPv4Address{239, 69, 1, 1}, .port = 5006}, net::FileDescriptor{}},  // a group already ruled
    }};
    std::array<XdpTransport::PortState*, 3> const ports{&states[0], &states[1], &states[2]};
    std::array<net::CompiledRule<32>, 3> untagged{};
    std::array<net::CompiledRule<32>, 3> tagged{};
    auto const plain = XdpTransport::compile_rules(ports, std::nullopt, untagged);
    auto const with_vid = XdpTransport::compile_rules(ports, 2, tagged);
    EXPECT_TRUE(plain.first == 2 && !plain.second);
    EXPECT_TRUE(with_vid.first == 3 && !with_vid.second);
    EXPECT_TRUE(same(tagged[0], untagged[0]) && same(tagged[1], untagged[1]));
    EXPECT_TRUE(same(tagged[2], XdpTransport::tagged_groups_rule(2)));
    // One slot short: the tagged rule keeps its slot, last, and the second group is the one dropped.
    std::array<net::CompiledRule<32>, 2> full{};
    auto const short_of_one = XdpTransport::compile_rules(ports, 2, full);
    EXPECT_TRUE(short_of_one.first == 2 && short_of_one.second == net::XdpError::too_many_filter_rules);
    EXPECT_TRUE(same(full[0], untagged[0]) && same(full[1], XdpTransport::tagged_groups_rule(2)));
}

TEST(aes67_xdp, a_port_awaiting_retirement_past_max_groups_drops_its_group_not_the_tagged_rule)
{
    auto const same = [](net::CompiledRule<32> const& a, net::CompiledRule<32> const& b) {
        return a.mask == b.mask && a.match == b.match && a.result_flags == b.result_flags;
    };
    // 17 groups: one more than MAX_GROUPS, as a closed port not yet retired leaves in ports_.
    std::vector<std::unique_ptr<XdpTransport::PortState>> states;
    std::vector<XdpTransport::PortState*> ports;
    for (uint8_t i = 0; i <= XdpTransport::MAX_GROUPS; ++i) {
        states.push_back(std::make_unique<XdpTransport::PortState>(
            Endpoint{.address = IPv4Address{239, 69, 2, i}, .port = 5004}, net::FileDescriptor{}));
        ports.push_back(states.back().get());
    }
    std::array<net::CompiledRule<32>, XdpTransport::MAX_GROUPS + 1> rules{};
    auto const [count, error] = XdpTransport::compile_rules(ports, 2, rules);
    EXPECT_EQ(count, XdpTransport::MAX_GROUPS + 1);
    EXPECT_TRUE(error == net::XdpError::too_many_filter_rules);  // what rules_status() reports
    EXPECT_TRUE(same(rules[XdpTransport::MAX_GROUPS], XdpTransport::tagged_groups_rule(2)));
    EXPECT_TRUE(same(rules[XdpTransport::MAX_GROUPS - 1], XdpTransport::group_rule(IPv4Address{239, 69, 2, 15})));
    auto const dropped = XdpTransport::group_rule(IPv4Address{239, 69, 2, 16});
    EXPECT_FALSE(std::ranges::any_of(rules, [&](net::CompiledRule<32> const& rule) { return same(rule, dropped); }));
}

TEST(aes67_xdp, a_tagged_frame_reaches_the_port_of_its_group)
{
    Endpoint const stream{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    XdpTransport::PortState port{stream, net::FileDescriptor{}};
    std::array<XdpTransport::PortState*, 1> const ports{&port};
    RefusalCounter refusals;
    EXPECT_TRUE(XdpTransport::dispatch_frame(rtp_frame_to(stream, ieee::make_vlan_tag(2, false, 3)), ports, {}, refusals));
    EXPECT_EQ(port.pending_count, size_t{1});
    // A tagged alias is told apart by its address.
    Endpoint const alias{.address = IPv4Address{239, 197, 1, 2}, .port = 5004};
    EXPECT_FALSE(XdpTransport::dispatch_frame(rtp_frame_to(alias, ieee::make_vlan_tag(2, false, 3)), ports, {}, refusals));
    EXPECT_EQ(refusals.load().unmatched, uint64_t{1});
}

TEST(aes67_xdp, a_send_port_s_tag_changes_from_its_next_frame)
{
    rtp::FrameAddressing const addressing{
        .destination_mac = rtp::multicast_mac(IPv4Address{239, 69, 1, 2}),
        .destination_address = IPv4Address{239, 69, 1, 2},
        .destination_port = 5004,
    };
    XdpTransport::SendAddressing sending{addressing};
    std::array<std::array<uint8_t, 512>, 3> buffers{};
    XdpTransport::TxPool pool;
    std::array<uint8_t, 288> const payload{};
    auto const tag = ieee::make_vlan_tag(2, false, 3);
    std::array<std::optional<ieee::VlanTag>, 3> const tags{std::nullopt, tag, std::nullopt};
    for (uint16_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(pool.put({.handle = i, .buffer = buffers[i]}));
        sending.set_vlan(tags[i]);
        rtp::RtpHeader header{};
        header.init(96, static_cast<uint16_t>(100 + i), 6U * i, 7);
        auto const written = XdpTransport::write_frame(pool, sending.current(), header, payload);
        size_t const length = written ? written->second : 0;
        auto const frame = rtp::parse_rtp_frame(std::span{buffers[i]}.first(length));
        EXPECT_EQ(length, rtp::rtp_frame_overhead(tags[i].has_value()) + rtp::RtpHeader::LENGTH + payload.size());
        EXPECT_TRUE(frame && frame->addressing.vlan == tags[i]);
        EXPECT_EQ(frame->packet.header.sequence.get(), uint16_t{static_cast<uint16_t>(100 + i)});
    }
    EXPECT_TRUE(XdpTransport::SendAddressing{rtp::FrameAddressing{.vlan = tag}}.current().vlan == std::optional{tag});
}

TEST(aes67_xdp, each_refused_frame_is_counted_under_its_reason)
{
    Endpoint const stream{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    XdpTransport::PortState port{stream, net::FileDescriptor{}};
    XdpTransport::RtcpPipe pipe{rtcp_endpoint_of(stream)};
    std::array<XdpTransport::PortState*, 1> const ports{&port};
    std::array<XdpTransport::RtcpPipe*, 1> const pipes{&pipe};
    RefusalCounter refusals;
    auto const offer = [&](std::vector<uint8_t> const& frame) {
        return XdpTransport::dispatch_frame(frame, ports, pipes, refusals);
    };

    auto const whole = rtp_frame_to(stream);
    EXPECT_TRUE(offer(whole));
    EXPECT_FALSE(offer(std::vector<uint8_t>(whole.begin(), whole.begin() + 100)));  // malformed: cut short
    EXPECT_FALSE(offer(rtp_frame_to({.address = stream.address, .port = 5008})));   // unmatched: no port
    EXPECT_FALSE(offer(rtp_frame_to(rtcp_endpoint_of(stream))));                    // the RTCP pipe's: not refused
    auto not_rtp = whole;
    not_rtp[rtp::rtp_frame_overhead(false)] = 0x00;  // RTP version 0
    EXPECT_FALSE(offer(not_rtp));
    for (size_t i = 1; i < XdpTransport::PORT_BATCH; ++i) {
        EXPECT_TRUE(offer(whole));
    }
    EXPECT_FALSE(offer(whole));  // port full

    auto const counted = refusals.load();
    EXPECT_EQ(counted.malformed, uint64_t{1});
    EXPECT_EQ(counted.unmatched, uint64_t{1});
    EXPECT_EQ(counted.not_rtp, uint64_t{1});
    EXPECT_EQ(counted.port_full, uint64_t{1});
    EXPECT_EQ(port.pending_count, XdpTransport::PORT_BATCH);
    EXPECT_EQ(pipe.dropped(), uint64_t{0});
    refusals.reset();
    EXPECT_EQ(refusals.load(), ReceiveRefusals{});
}

TEST(aes67_xdp, a_frame_two_ports_share_is_refused_once_and_only_when_none_keeps_it)
{
    Endpoint const stream{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    XdpTransport::PortState full{stream, net::FileDescriptor{}};
    XdpTransport::PortState open{stream, net::FileDescriptor{}};
    full.pending_count = XdpTransport::PORT_BATCH;
    std::array<XdpTransport::PortState*, 2> const ports{&full, &open};
    RefusalCounter refusals;
    auto const whole = rtp_frame_to(stream);
    EXPECT_TRUE(XdpTransport::dispatch_frame(whole, ports, {}, refusals));  // the open port keeps it
    auto not_rtp = whole;
    not_rtp[rtp::rtp_frame_overhead(false)] = 0x00;
    EXPECT_FALSE(XdpTransport::dispatch_frame(not_rtp, ports, {}, refusals));
    auto rtcp = whole;
    rtcp[rtp::rtp_frame_overhead(false) + 1] = 200;  // the stream's own SR on its RTP port (RFC 5761)
    EXPECT_FALSE(XdpTransport::dispatch_frame(rtcp, ports, {}, refusals));
    open.pending_count = XdpTransport::PORT_BATCH;
    EXPECT_FALSE(XdpTransport::dispatch_frame(whole, ports, {}, refusals));
    auto const counted = refusals.load();
    EXPECT_EQ(counted.not_rtp, uint64_t{1});
    EXPECT_EQ(counted.port_full, uint64_t{1});
    EXPECT_EQ(counted.unmatched + counted.malformed, uint64_t{0});
}

TEST(aes67_xdp, a_closed_port_takes_nothing_and_its_frames_are_unmatched)
{
    Endpoint const stream{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    XdpTransport::PortState port{stream, net::FileDescriptor{}};
    port.closed.store(true);
    std::array<XdpTransport::PortState*, 1> const ports{&port};
    RefusalCounter refusals;
    EXPECT_FALSE(XdpTransport::dispatch_frame(rtp_frame_to(stream), ports, {}, refusals));
    EXPECT_EQ(refusals.load().unmatched, uint64_t{1});
    EXPECT_EQ(port.pending_count, size_t{0});
}

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
    // A reset takes the socket's counters at the next end_tick(), on the ticking thread.
    (*transport)->reset_counters();
    EXPECT_EQ((*transport)->socket_stats().tx_count, uint64_t{1});
    (*transport)->begin_tick();
    (*transport)->end_tick();
    EXPECT_EQ((*transport)->socket_stats().tx_count, uint64_t{0});
    EXPECT_EQ((*transport)->socket_stats().tx_flush_count + (*transport)->socket_stats().tx_flush_skipped, uint64_t{0});
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
