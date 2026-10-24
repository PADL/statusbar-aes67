#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// Socket plumbing the transports share: interface lookup and sockaddr conversion. Sockets and group
/// joins are net_socket.hpp's; dotted quads are ip::parse_ipv4() and ip::to_string().

#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/status/status.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <netinet/in.h>

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

}  // namespace statusbar::aes67
