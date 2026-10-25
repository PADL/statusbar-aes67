// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// Two UdpTransports on two veth pairs receiving the same group: a datagram arriving on one
/// interface reaches only that interface's RTP and RTCP ports. Needs root (it creates the
/// pairs and sends raw frames); skips without it.

#include "statusbar/aes67/aes67_udp_transport.hpp"
#include "statusbar/rtp/rtp_frame.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::aes67;
using statusbar::ip::IPv4Address;

namespace {

/// A veth pair, its first end addressed, both up; removed on destruction.
class VethPair
{
  public:
    VethPair(std::string near, std::string const& far, std::string const& address)
        : near_{std::move(near)}
    {
        (void)std::system(std::format("ip link del {} 2>/dev/null", near_).c_str());
        ok_ = std::system(std::format(
                              "ip link add {0} type veth peer name {1} && ip addr add {2} dev {0} && ip link set {0} up && "
                              "ip link set {1} up",
                              near_,
                              far,
                              address)
                              .c_str()) == 0;
    }
    ~VethPair() { (void)std::system(std::format("ip link del {} 2>/dev/null", near_).c_str()); }
    VethPair(VethPair const&) = delete;
    auto operator=(VethPair const&) -> VethPair& = delete;
    VethPair(VethPair&&) = delete;
    auto operator=(VethPair&&) -> VethPair& = delete;

    [[nodiscard]] auto ok() const noexcept -> bool { return ok_; }

  private:
    std::string near_;
    bool ok_{false};
};

/// Send one UDP datagram to `group`:`port` from `source` as a raw frame on `interface`.
auto inject(std::string const& interface, IPv4Address const& source, IPv4Address const& group, uint16_t const port) -> bool
{
    net::FileDescriptor const fd{::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP))};
    rtp::FrameAddressing const addressing{
        .destination_mac = rtp::multicast_mac(group),
        .source_mac = ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x02},
        .source_address = source,
        .destination_address = group,
        .source_port = port,
        .destination_port = port,
    };
    rtp::RtpHeader header{};
    header.init(97, 1, 2, 3);
    std::array<uint8_t, 12> const payload{};
    std::array<uint8_t, 128> frame{};
    auto const length = rtp::serialize_rtp_frame(addressing, header, payload, frame);
    sockaddr_ll address{};
    address.sll_family = AF_PACKET;
    address.sll_protocol = htons(ETH_P_IP);
    address.sll_ifindex = static_cast<int>(::if_nametoindex(interface.c_str()));
    address.sll_halen = ETH_ALEN;
    return fd.get() >= 0 && length &&
        ::sendto(fd.get(), frame.data(), *length, 0, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) ==
        static_cast<ssize_t>(*length);
}

/// The datagrams `port` holds after 20 ms.
template <typename Port>
auto received(Port& port) -> size_t
{
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    std::array<Datagram, 8> batch{};
    size_t total = 0;
    for (size_t n = port.receive(batch); n > 0; n = port.receive(batch)) {
        total += n;
    }
    return total;
}

}  // namespace

TEST(aes67_veth, a_group_joined_on_two_interfaces_is_received_only_where_it_arrives)
{
    if (::geteuid() != 0) {
        std::println("  skipped: needs root to create veth pairs");
        return;
    }
    VethPair const first{"a67veth0", "a67veth1", "10.232.0.1/24"};
    VethPair const second{"a67veth2", "a67veth3", "10.232.1.1/24"};
    if (!first.ok() || !second.ok()) {
        std::println("  skipped: cannot create veth pairs");
        return;
    }
    auto first_transport = UdpTransport::open("a67veth0");
    auto second_transport = UdpTransport::open("a67veth2");
    EXPECT_TRUE(first_transport.has_value() && second_transport.has_value());
    if (!first_transport || !second_transport) {
        return;
    }
    IPv4Address const group{239, 69, 232, 1};
    Endpoint const rtp{.address = group, .port = 45200};
    RtcpOptions const rtcp{.destination = {.address = group, .port = 45201}};
    auto first_rtp = (*first_transport)->open_receive(rtp);
    auto second_rtp = (*second_transport)->open_receive(rtp);
    auto first_rtcp = (*first_transport)->open_rtcp(rtcp);
    auto second_rtcp = (*second_transport)->open_rtcp(rtcp);
    EXPECT_TRUE(first_rtp && second_rtp && first_rtcp && second_rtcp);
    if (!first_rtp || !second_rtp || !first_rtcp || !second_rtcp) {
        return;
    }

    EXPECT_TRUE(inject("a67veth1", IPv4Address{10, 232, 0, 2}, group, rtp.port));
    EXPECT_TRUE(inject("a67veth1", IPv4Address{10, 232, 0, 2}, group, rtcp.destination.port));
    EXPECT_EQ(received(**first_rtp), size_t{1});
    EXPECT_EQ(received(**second_rtp), size_t{0});
    EXPECT_EQ(received(**first_rtcp), size_t{1});
    EXPECT_EQ(received(**second_rtcp), size_t{0});

    EXPECT_TRUE(inject("a67veth3", IPv4Address{10, 232, 1, 2}, group, rtp.port));
    EXPECT_TRUE(inject("a67veth3", IPv4Address{10, 232, 1, 2}, group, rtcp.destination.port));
    EXPECT_EQ(received(**first_rtp), size_t{0});
    EXPECT_EQ(received(**second_rtp), size_t{1});
    EXPECT_EQ(received(**first_rtcp), size_t{0});
    EXPECT_EQ(received(**second_rtcp), size_t{1});
    EXPECT_EQ((*first_rtcp)->dropped() + (*second_rtcp)->dropped(), uint64_t{0});
}

TEST_MAIN(statusbar_aes67, aes67_veth_test)
