// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_socket.hpp"

#include "statusbar/aes67/aes67_error.hpp"

#include <ifaddrs.h>

#include <cerrno>
#include <cstring>
#include <format>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>

namespace statusbar::aes67 {

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

}  // namespace statusbar::aes67
