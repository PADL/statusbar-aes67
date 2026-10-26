// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_socket.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/net/net_posix_util.hpp"

#include <ifaddrs.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <format>
#include <memory>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::aes67 {

namespace {

class UdpRtcpPort final : public RtcpPort
{
  public:
    /// Datagrams one receive() takes at most.
    static constexpr size_t BATCH = 8;
    /// The largest datagram kept (an Ethernet MTU's worth).
    static constexpr size_t MAX_DATAGRAM = 1500;

    UdpRtcpPort(
        net::FileDescriptor fd,
        sockaddr_in const& destination,
        ip::IPv4Address const& local,
        unsigned int const interface_index) noexcept
        : fd_{std::move(fd)}
        , destination_{destination}
        , local_{local}
        , interface_index_{interface_index}
    {}

    [[nodiscard]] auto fd() const noexcept -> int override { return fd_.get(); }

    auto receive(std::span<Datagram> const out) noexcept -> size_t override
    {
        // At most `want` datagrams read per call, refused ones included, so a flood cannot hold the reactor.
        size_t const want = std::min(out.size(), BATCH);
        size_t written = 0;
        for (size_t reads = 0; reads < want; ++reads) {
            sockaddr_in source{};
            iovec iov{.iov_base = buffers_[written].data(), .iov_len = buffers_[written].size()};
            alignas(cmsghdr) std::array<uint8_t, PKTINFO_SPACE> control{};
            msghdr header{};
            header.msg_name = &source;
            header.msg_namelen = sizeof(source);
            header.msg_iov = &iov;
            header.msg_iovlen = 1;
            header.msg_control = control.data();
            header.msg_controllen = control.size();
            ssize_t const got = ::recvmsg(fd_.get(), &header, MSG_DONTWAIT);
            if (got < 0) {
                break;
            }
            if ((header.msg_flags & MSG_TRUNC) != 0 || !destined_for(header, local_, interface_index_)) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            out[written] = {
                .payload = std::span<uint8_t const>{buffers_[written]}.first(static_cast<size_t>(got)),
                .source = from_sockaddr(source),
            };
            ++written;
        }
        return written;
    }

    auto send(std::span<uint8_t const> const packet) noexcept -> Status override
    {
        ssize_t const sent =
            ::sendto(fd_.get(), packet.data(), packet.size(), MSG_DONTWAIT, net::sockaddr_cast(destination_), sizeof(destination_));
        if (sent < 0) {
            return failure(net::errno_code());
        }
        return success();
    }

    [[nodiscard]] auto dropped() const noexcept -> uint64_t override { return dropped_.load(std::memory_order_relaxed); }

  private:
    net::FileDescriptor fd_;
    sockaddr_in destination_;
    ip::IPv4Address local_;         ///< the bound address: the group, or this host's
    unsigned int interface_index_;  ///< the group's interface, or 0 for a unicast port
    std::array<std::array<uint8_t, MAX_DATAGRAM>, BATCH> buffers_{};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace

auto find_interface(std::string_view const name) -> StatusValue<InterfaceInfo>
{
    InterfaceInfo info{.name = std::string{name}};
    info.index = ::if_nametoindex(info.name.c_str());
    if (info.index == 0) {
        return failure(Aes67Error::interface_not_found);
    }
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return failure(net::errno_code());
    }
    bool found = false;
    for (ifaddrs const* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET || info.name != entry->ifa_name) {
            continue;
        }
        sockaddr_in address{};
        std::memcpy(&address, entry->ifa_addr, sizeof(address));
        info.address = from_sockaddr(address).address;
        found = true;
        break;
    }
    ::freeifaddrs(list);
    if (!found) {
        return failure(Aes67Error::interface_not_found);
    }
    return info;
}

auto to_sockaddr(Endpoint const& endpoint) noexcept -> sockaddr_in
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    address.sin_addr.s_addr = htonl(endpoint.address.to_uint32());
    return address;
}

auto from_sockaddr(sockaddr_in const& address) noexcept -> Endpoint
{
    return {.address = ip::IPv4Address{ntohl(address.sin_addr.s_addr)}, .port = ntohs(address.sin_port)};
}

auto destined_for(msghdr& header, ip::IPv4Address const& address, unsigned int const interface_index) noexcept -> bool
{
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&header); cmsg != nullptr; cmsg = CMSG_NXTHDR(&header, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
            in_pktinfo info{};
            std::memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
            return ntohl(info.ipi_addr.s_addr) == address.to_uint32() &&
                (interface_index == 0 || static_cast<unsigned int>(info.ipi_ifindex) == interface_index);
        }
    }
    return false;
}

auto open_receive_socket(InterfaceInfo const& interface, Endpoint const& local, bool const shared)
    -> StatusValue<net::FileDescriptor>
{
    auto fd = net::make_udp_socket();
    if (!fd) {
        return failure(fd.error());
    }
    if (shared) {
        if (auto set = net::set_reuse_addr(fd->get()); !set) {
            return failure(set.error());
        }
    }
    int const one = 1;
    if (::setsockopt(fd->get(), IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) != 0) {
        return failure(net::errno_code());
    }
    bool const multicast = local.address.is_multicast();
    int const all = 0;
    if (multicast && ::setsockopt(fd->get(), IPPROTO_IP, IP_MULTICAST_ALL, &all, sizeof(all)) != 0) {
        return failure(net::errno_code());
    }
    sockaddr_in const address = to_sockaddr(local);
    if (::bind(fd->get(), net::sockaddr_cast(address), sizeof(address)) != 0) {
        return failure(net::errno_code());
    }
    if (multicast) {
        if (auto joined = net::join_group(fd->get(), local.address, interface.index); !joined) {
            return failure(joined.error());
        }
    }
    return std::move(*fd);
}

auto open_rtcp_socket(InterfaceInfo const& interface, RtcpOptions const& options) -> StatusValue<std::unique_ptr<RtcpPort>>
{
    bool const multicast = options.destination.address.is_multicast();
    Endpoint local = options.local;
    if (multicast) {
        local = options.destination;
    } else {
        local.address = local.address == ip::IPv4Address{} ? interface.address : local.address;
        local.port = local.port == 0 ? options.destination.port : local.port;
    }
    // Groups share their port (another RTCP session, an RTP receiver of another stream); a unicast
    // address and port is this port's alone, so a second bind fails with address_in_use.
    auto fd = open_receive_socket(interface, local, multicast);
    if (!fd) {
        return failure(fd.error());
    }
    if (multicast) {
        if (auto set = net::set_multicast_send(fd->get(), interface.index, options.ttl, true); !set) {
            return failure(set.error());
        }
    }
    if (auto set = net::set_dscp(fd->get(), AF_INET, options.dscp); !set) {
        return failure(set.error());
    }
    return std::make_unique<UdpRtcpPort>(
        std::move(*fd), to_sockaddr(options.destination), local.address, multicast ? interface.index : 0U);
}

}  // namespace statusbar::aes67
