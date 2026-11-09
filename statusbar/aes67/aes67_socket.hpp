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
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::aes67 {

/// An interface's index and its IPv4 addresses, the first of them `address`.
struct InterfaceInfo
{
    std::string name;
    unsigned int index{0};
    ip::IPv4Address address;
    std::vector<ip::IPv4Address> addresses;
    bool loopback{false};            ///< IFF_LOOPBACK: every 127/8 address is its own
    std::optional<ieee::Eui48> mac;  ///< its hardware address; nullopt for none (or all zeros, as loopback's)
};

/// Whether `address` is one of `interface`'s own: among its addresses, or any 127/8 address on a loopback interface.
[[nodiscard]] auto is_local(InterfaceInfo const& interface, ip::IPv4Address const& address) noexcept -> bool;

/// Look up an interface by name: it must exist and have an IPv4 address.
[[nodiscard]] auto find_interface(std::string_view name) -> StatusValue<InterfaceInfo>;

[[nodiscard]] auto to_sockaddr(Endpoint const& endpoint) noexcept -> sockaddr_in;
[[nodiscard]] auto from_sockaddr(sockaddr_in const& address) noexcept -> Endpoint;

/// The ancillary space recvmsg() needs for IP_PKTINFO.
inline constexpr size_t PKTINFO_SPACE = CMSG_SPACE(sizeof(in_pktinfo));

/// Whether a received message's IP_PKTINFO names `address` as its destination and, unless
/// `interface_index` is 0, `interface_index` as the interface it arrived on; false without one.
[[nodiscard]] auto destined_for(msghdr& header, ip::IPv4Address const& address, unsigned int interface_index = 0) noexcept -> bool;

/// A UDP socket bound to `local` with IP_PKTINFO on (SO_REUSEADDR when `shared`). A group is joined on `interface`,
/// for `source` alone when given (RFC 4607), with IP_MULTICAST_ALL off, so only this socket's joins reach it.
[[nodiscard]] auto open_receive_socket(
    InterfaceInfo const& interface, Endpoint const& local, bool shared, std::optional<ip::IPv4Address> const& source = std::nullopt)
    -> StatusValue<net::FileDescriptor>;

/// A sender's unconnected UDP socket, marked with `options.dscp`: to a group out of `interface` with `options.ttl`
/// and loopback; to a unicast destination bound to `interface` and its address (IP_UNICAST_IF) with `options.ttl`,
/// at socket priority 0, so the DSCP never maps it to an SR class's queue (AES67-2023 Annex C.1.2).
[[nodiscard]] auto open_send_socket(InterfaceInfo const& interface, SendOptions const& options) -> StatusValue<net::FileDescriptor>;

/// An RTCP port on a kernel UDP socket: bound to the group (joined, for RtcpOptions::source alone when given; sent to
/// with `options.ttl` and loopback) or to RtcpOptions::local (by default `interface`'s address; out of `interface` with
/// RtcpOptions::ttl at socket priority 0, as a unicast stream's), marked with RtcpOptions::dscp.
[[nodiscard]] auto open_rtcp_socket(InterfaceInfo const& interface, RtcpOptions const& options)
    -> StatusValue<std::unique_ptr<RtcpPort>>;

}  // namespace statusbar::aes67
