#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// Socket plumbing the transports share: interface lookup, sockaddr conversion and the RTCP socket.
/// Sockets and group joins are net_socket.hpp's; dotted quads are ip::parse_ipv4() and ip::to_string().

#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/status/status.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::aes67 {

/// An interface's index and its first IPv4 address.
struct InterfaceInfo
{
    std::string name;
    unsigned int index{0};
    ip::IPv4Address address;
};

/// Look up an interface by name: it must exist and have an IPv4 address.
[[nodiscard]] auto find_interface(std::string_view name) -> StatusValue<InterfaceInfo>;

[[nodiscard]] auto to_sockaddr(Endpoint const& endpoint) noexcept -> sockaddr_in;
[[nodiscard]] auto from_sockaddr(sockaddr_in const& address) noexcept -> Endpoint;

/// The ancillary space recvmsg() needs for IP_PKTINFO.
inline constexpr size_t PKTINFO_SPACE = CMSG_SPACE(sizeof(in_pktinfo));

/// Whether a received message's IP_PKTINFO names `address` as its destination and, unless
/// `interface_index` is 0, `interface_index` as the interface it arrived on; false without one.
[[nodiscard]] auto destined_for(msghdr& header, ip::IPv4Address const& address, unsigned int interface_index = 0) noexcept -> bool;

/// A UDP socket bound to `local` with IP_PKTINFO on (SO_REUSEADDR when `shared`). A group is joined
/// on `interface` with IP_MULTICAST_ALL off, so only this socket's joins, on their interfaces, reach it.
[[nodiscard]] auto open_receive_socket(InterfaceInfo const& interface, Endpoint const& local, bool shared)
    -> StatusValue<net::FileDescriptor>;

/// An RTCP port on a kernel UDP socket: bound to the group (joined, sent to with `options.ttl` and
/// loopback) or to RtcpOptions::local (by default `interface`'s address), marked with RtcpOptions::dscp.
[[nodiscard]] auto open_rtcp_socket(InterfaceInfo const& interface, RtcpOptions const& options)
    -> StatusValue<std::unique_ptr<RtcpPort>>;

}  // namespace statusbar::aes67
