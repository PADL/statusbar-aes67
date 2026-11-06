// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_split_transport.hpp"

#include "statusbar/aes67/aes67_receiver.hpp"
#include "statusbar/aes67/aes67_sender.hpp"
#include "statusbar/aes67/aes67_udp_transport.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/rtp/rtp_header.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::aes67;
using statusbar::ip::IPv4Address;

namespace {

IPv4Address const GROUP{239, 69, 7, 1};
IPv4Address const PEER{10, 0, 0, 7};

/// A transport that records what it was asked to open and how it was ticked.
class RecordingTransport final : public Transport
{
  public:
    class Rx final : public ReceivePort
    {
      public:
        auto receive(std::span<Datagram> /*out*/) noexcept -> size_t override { return 0; }
    };
    class Tx final : public SendPort
    {
      public:
        auto send(rtp::RtpHeader const& /*header*/, std::span<uint8_t const> /*payload*/) noexcept -> bool override { return true; }
        auto send_rendered(rtp::RtpHeader const& /*header*/, size_t /*length*/, PayloadRenderer const& /*render*/) noexcept
            -> bool override
        {
            return true;
        }
        auto flush() noexcept -> size_t override { return 0; }
    };
    class Rtcp final : public RtcpPort
    {
      public:
        [[nodiscard]] auto fd() const noexcept -> int override { return -1; }
        auto receive(std::span<Datagram> /*out*/) noexcept -> size_t override { return 0; }
        auto send(std::span<uint8_t const> /*packet*/) noexcept -> Status override { return success(); }
    };

    explicit RecordingTransport(bool const every_tick) noexcept
        : every_tick_{every_tick}
    {}

    [[nodiscard]] auto local_address() const noexcept -> IPv4Address override { return IPv4Address{10, 0, 0, 1}; }
    auto open_receive(Endpoint const& destination) -> StatusValue<std::unique_ptr<ReceivePort>> override
    {
        opened.push_back(destination.address);
        return std::make_unique<Rx>();
    }
    auto open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>> override
    {
        opened.push_back(options.destination.address);
        tagged = options.vlan.has_value();
        return std::make_unique<Tx>();
    }
    auto open_rtcp(RtcpOptions const& options) -> StatusValue<std::unique_ptr<RtcpPort>> override
    {
        opened.push_back(options.destination.address);
        return std::make_unique<Rtcp>();
    }
    void begin_tick() noexcept override { ++begun; }
    void end_tick() noexcept override { ++ended; }
    [[nodiscard]] auto drain_every_tick(Endpoint const& /*destination*/) const noexcept -> bool override { return every_tick_; }
    [[nodiscard]] auto send_error() const noexcept -> std::error_code override { return failing; }
    [[nodiscard]] auto receive_refusals() const noexcept -> ReceiveRefusals override { return refusals; }
    void reset_counters() noexcept override { refusals = {}; }

    std::vector<IPv4Address> opened;
    bool tagged{false};  ///< the last open_send() asked for a tag
    int begun{0};
    int ended{0};
    std::error_code failing;
    ReceiveRefusals refusals;

  private:
    bool every_tick_;
};

struct Pair
{
    RecordingTransport* multicast;
    RecordingTransport* unicast;
    std::unique_ptr<SplitTransport> split;
};

auto make_pair() -> Pair
{
    auto multicast = std::make_unique<RecordingTransport>(true);
    auto unicast = std::make_unique<RecordingTransport>(false);
    Pair pair{.multicast = multicast.get(), .unicast = unicast.get(), .split = nullptr};
    pair.split = std::make_unique<SplitTransport>(std::move(multicast), std::move(unicast));
    return pair;
}

}  // namespace

TEST(aes67_split_transport, groups_go_to_the_multicast_transport_and_unicast_addresses_to_the_other)
{
    auto const [multicast, unicast, split] = make_pair();
    EXPECT_TRUE(split->open_receive({.address = GROUP, .port = 5004}).has_value());
    EXPECT_TRUE(split->open_send({.destination = {.address = PEER, .port = 5004}}).has_value());
    EXPECT_TRUE(split->open_rtcp({.destination = {.address = GROUP, .port = 5005}}).has_value());
    EXPECT_TRUE(split->open_receive({.address = IPv4Address{10, 0, 0, 1}, .port = 5006}).has_value());
    EXPECT_TRUE(split->open_send({.destination = {.address = GROUP, .port = 5004}}).has_value());
    EXPECT_TRUE(split->open_rtcp({.destination = {.address = PEER, .port = 5007}}).has_value());
    EXPECT_TRUE(multicast->opened == (std::vector<IPv4Address>{GROUP, GROUP, GROUP}));
    EXPECT_TRUE(unicast->opened == (std::vector<IPv4Address>{PEER, IPv4Address{10, 0, 0, 1}, PEER}));
    EXPECT_TRUE(split->local_address() == (IPv4Address{10, 0, 0, 1}));
    EXPECT_TRUE(split->is_local_address(IPv4Address{10, 0, 0, 1}) && !split->is_local_address(PEER));
}

TEST(aes67_split_transport, a_tick_brackets_both_transports)
{
    auto const [multicast, unicast, split] = make_pair();
    split->begin_tick();
    split->end_tick();
    EXPECT_EQ(multicast->begun + unicast->begun, 2);
    EXPECT_EQ(multicast->ended + unicast->ended, 2);
}

TEST(aes67_split_transport, a_unicast_port_keeps_the_due_tick_gate_beside_xdp_groups)
{
    auto const [multicast, unicast, split] = make_pair();
    EXPECT_TRUE(split->drain_every_tick({.address = GROUP, .port = 5004}));
    EXPECT_FALSE(split->drain_every_tick({.address = PEER, .port = 5004}));
}

TEST(aes67_split_transport, refusals_are_summed_and_reset_on_both)
{
    auto const [multicast, unicast, split] = make_pair();
    multicast->refusals = {.malformed = 1, .unmatched = 2, .not_rtp = 3, .port_full = 4};
    unicast->refusals = {.malformed = 10, .unmatched = 0, .not_rtp = 0, .port_full = 0};
    EXPECT_TRUE(split->receive_refusals() == (ReceiveRefusals{.malformed = 11, .unmatched = 2, .not_rtp = 3, .port_full = 4}));
    split->reset_counters();
    EXPECT_TRUE(split->receive_refusals() == ReceiveRefusals{});
}

TEST(aes67_split_transport, a_send_error_is_the_multicast_transport_s_before_the_unicast_one_s)
{
    auto const [multicast, unicast, split] = make_pair();
    EXPECT_FALSE(split->send_error());
    unicast->failing = make_error_code(std::errc::network_down);
    EXPECT_TRUE(split->send_error() == std::errc::network_down);
    multicast->failing = make_error_code(std::errc::no_buffer_space);
    EXPECT_TRUE(split->send_error() == std::errc::no_buffer_space);
    // each sender is told its own side's: a failing AF_XDP kick is not a unicast sender's
    unicast->failing.clear();
    EXPECT_TRUE(split->send_error_for({.address = GROUP, .port = 5004}) == std::errc::no_buffer_space);
    EXPECT_FALSE(split->send_error_for({.address = PEER, .port = 5004}));
}

// A tagged output moved to a unicast destination (its tag carried over) opens untagged: a reservation is to a group.
TEST(aes67_split_transport, a_unicast_send_port_is_opened_untagged)
{
    auto const [multicast, unicast, split] = make_pair();
    auto const tag = ieee::make_vlan_tag(3, false, 2);
    EXPECT_TRUE(split->open_send({.destination = {.address = GROUP, .port = 5004}, .vlan = tag}).has_value());
    EXPECT_TRUE(split->open_send({.destination = {.address = PEER, .port = 5004}, .vlan = tag}).has_value());
    EXPECT_TRUE(multicast->tagged);
    EXPECT_FALSE(unicast->tagged);
}

// A unicast stream over the split's kernel sockets on loopback: the sender's own SDP, received at its c= address,
// polled behind the due-tick gate while the multicast side drains every tick.
TEST(aes67_split_transport, a_unicast_stream_crosses_the_kernel_side_end_to_end)
{
    auto udp = UdpTransport::open("lo");
    EXPECT_TRUE(udp.has_value());
    if (!udp) {
        return;
    }
    SplitTransport split{std::make_unique<RecordingTransport>(true), std::move(*udp)};
    auto const pid = static_cast<uint32_t>(::getpid());
    IPv4Address const receiver_address{127, 99, static_cast<uint8_t>(pid >> 8U), static_cast<uint8_t>(pid)};
    StreamFormat const format{.encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = 48};
    Sender sender{format, [](uint64_t, uint32_t, std::span<uint8_t>) { return Supply::none; }, split, "Unicast"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = receiver_address, .port = 45304, .payload_type = 97}));
    uint32_t packets = 0;
    Receiver receiver{format, [&packets](uint64_t, uint32_t, std::span<uint8_t const>) { ++packets; }, split};
    EXPECT_TRUE(receiver.configure(*sender.session()));
    uint64_t now = (48'000ULL * 1000) + 47;
    for (int step = 0; step < 4; ++step, now += 48) {
        split.begin_tick();
        sender.tick(now, 0);
        split.end_tick();
    }
    for (int tries = 0; tries < 100 && packets < 4; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        receiver.drain(now);
    }
    EXPECT_EQ(packets, uint32_t{4});
    EXPECT_EQ(sender.counters().packets, uint64_t{4});
}

/// One RTP packet of 48 L24 stereo frames sent to 127.0.0.1:`port` from a socket bound to `from`.
void send_rtp_from(IPv4Address const& from, uint16_t const port)
{
    auto socket = net::make_udp_socket();
    EXPECT_TRUE(socket.has_value());
    sockaddr_in const source = to_sockaddr({.address = from, .port = 0});
    EXPECT_EQ(::bind(socket->get(), reinterpret_cast<sockaddr const*>(&source), sizeof(source)), 0);
    rtp::RtpHeader header{};
    header.init(97, 1, 48'000U * 1000, 0x5151);
    std::array<uint8_t, rtp::RtpHeader::LENGTH + (48 * 6)> packet{};
    EXPECT_TRUE(rtp::serialize_rtp(header, std::span{packet}.subspan(rtp::RtpHeader::LENGTH), packet).has_value());
    sockaddr_in const to = to_sockaddr({.address = IPv4Address{127, 0, 0, 1}, .port = port});
    auto const sent = ::sendto(socket->get(), packet.data(), packet.size(), 0, reinterpret_cast<sockaddr const*>(&to), sizeof(to));
    EXPECT_EQ(sent, static_cast<ssize_t>(packet.size()));
}

// The offer branch over the kernel side: c= equal to o=, another host's address, is bound at the interface's
// address and filtered to that sender, so a packet from any other source is refused (on loopback nothing else can
// be sent unprivileged; the veth test sends the sender's).
TEST(aes67_split_transport, an_offer_is_bound_at_the_interface_and_filtered_to_its_sender_over_the_kernel_side)
{
    auto udp = UdpTransport::open("lo");
    EXPECT_TRUE(udp.has_value());
    if (!udp) {
        return;
    }
    SplitTransport split{std::make_unique<RecordingTransport>(true), std::move(*udp)};
    IPv4Address const sender_address{192, 0, 2, 9};
    sdp::Session offer{.origin_address = "192.0.2.9", .connection = sdp::Connection{.address = sender_address}};
    offer.media.push_back(
        {.port = 45306,
         .payload_type = 97,
         .encoding = sdp::Encoding::l24,
         .channels = 2,
         .media_clock = sdp::MediaClock::direct(0)});
    Endpoint const here{.address = IPv4Address{127, 0, 0, 1}, .port = 45306};
    EXPECT_TRUE(receive_endpoint(offer, offer.media[0], *offer.connection, split) == here);
    uint32_t packets = 0;
    StreamFormat const format{.encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = 48};
    Receiver receiver{format, [&packets](uint64_t, uint32_t, std::span<uint8_t const>) { ++packets; }, split};
    EXPECT_TRUE(receiver.configure(offer).has_value());
    send_rtp_from(IPv4Address{127, 0, 0, 1}, here.port);
    for (int tries = 0; tries < 100 && receiver.counters().drops.source < 1; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        receiver.drain(48'000ULL * 1000);
    }
    EXPECT_EQ(receiver.counters().drops.source, uint64_t{1});  // not the sender's: refused by the filter c= gave
    EXPECT_EQ(packets, uint32_t{0});
}

TEST_MAIN(statusbar_aes67, aes67_split_transport_test)
