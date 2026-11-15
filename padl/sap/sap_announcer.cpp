// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/sap/sap_announcer.hpp"

#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/net/net_socket.hpp"

#include <algorithm>
#include <functional>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace padl::sap {

namespace {

/// The message id hash of `sdp`, never `previous` (RFC 2974 Section 5: a modified session has a new one) nor 0,
/// which means "no hash".
auto hash_for(std::string_view const sdp, uint16_t const previous = 0) noexcept -> uint16_t
{
    auto const h = std::hash<std::string_view>{}(sdp);
    auto value = static_cast<uint16_t>(h ^ (h >> 16U) ^ (h >> 32U) ^ (h >> 48U));
    value = value == previous ? static_cast<uint16_t>(previous + 1) : value;
    return value == 0 ? uint16_t{1} : value;
}

/// An IPv4 socket address.
[[nodiscard]] auto socket_address(statusbar::ip::IPv4Address const& address, uint16_t const port) noexcept -> sockaddr_in
{
    sockaddr_in socket = statusbar::net::ipv4_sockaddr_in(address);
    socket.sin_port = htons(port);
    return socket;
}

/// Binds `fd` to `origin`, so packets leave from the address their origin field names (RFC 2974 Section 6), and
/// connects it to `destination`:`port`, so send() needs no address.
[[nodiscard]] auto connect_from(
    int const fd,
    statusbar::ip::IPv4Address const& origin,
    statusbar::ip::IPv4Address const& destination,
    uint16_t const port) noexcept -> statusbar::Status
{
    auto const from = socket_address(origin, 0);
    auto const to = socket_address(destination, port);
    if (::bind(fd, statusbar::net::sockaddr_cast(from), sizeof(from)) != 0 ||
        ::connect(fd, statusbar::net::sockaddr_cast(to), sizeof(to)) != 0) {
        return statusbar::failure(statusbar::net::errno_code());
    }
    return statusbar::success();
}

}  // namespace

auto SapAnnouncer::open(
    unsigned int const interface_index,
    statusbar::ip::IPv4Address const& origin,
    statusbar::ip::IPv4Address const& destination,
    uint16_t const port,
    uint8_t const ttl) -> statusbar::StatusValue<std::unique_ptr<SapAnnouncer>>
{
    auto fd = statusbar::net::make_udp_socket();
    if (!fd) {
        return statusbar::failure(fd.error());
    }
    if (destination.is_multicast()) {
        if (auto set = statusbar::net::set_multicast_send(fd->get(), interface_index, ttl, true); !set) {
            return statusbar::failure(set.error());
        }
    }
    if (auto connected = connect_from(fd->get(), origin, destination, port); !connected) {
        return statusbar::failure(connected.error());
    }
    return std::make_unique<SapAnnouncer>(std::move(*fd), origin);
}

SapAnnouncer::SapAnnouncer(statusbar::net::FileDescriptor fd, statusbar::ip::IPv4Address const& origin) noexcept
    : fd_{std::move(fd)}
    , origin_{origin}
{}

void SapAnnouncer::announce(std::string const& key, sdp::Session const& session)
{
    std::string text = sdp::to_string(session);
    auto const it = std::ranges::find(entries_, key, &Entry::key);
    if (it != entries_.end()) {
        if (it->sdp == text) {
            return;
        }
        it->sdp = std::move(text);  // RFC 2974 Section 5: re-announced under a new hash, no deletion
        it->hash = hash_for(it->sdp, it->hash);
        it->due = true;
        return;
    }
    Entry entry{.key = key, .sdp = std::move(text)};
    entry.hash = hash_for(entry.sdp);
    entries_.push_back(std::move(entry));
}

auto SapAnnouncer::withdraw(std::string const& key) -> statusbar::Status
{
    auto const it = std::ranges::find(entries_, key, &Entry::key);
    if (it == entries_.end()) {
        return statusbar::success();
    }
    auto status = send(true, *it);
    entries_.erase(it);
    return status;
}

auto SapAnnouncer::withdraw_all() -> statusbar::StatusValue<size_t>
{
    auto const entries = std::exchange(entries_, {});
    size_t sent = 0;
    statusbar::Status first = statusbar::success();
    for (auto const& entry : entries) {
        if (auto status = send(true, entry); status) {
            ++sent;
        } else if (first) {
            first = status;
        }
    }
    if (!first) {
        return statusbar::failure(first.error());
    }
    return sent;
}

void SapAnnouncer::tick(int64_t const now_ns)
{
    for (auto& entry : entries_) {
        if (entry.due || now_ns >= entry.next_ns) {
            (void)send(false, entry);
            entry.due = false;
            entry.next_ns = now_ns + INTERVAL_NS;
        }
    }
}

auto SapAnnouncer::send(bool const deletion, Entry const& entry) -> statusbar::Status
{
    SapHeader header{};
    header.init(deletion, entry.hash, origin_);
    auto const length = serialize_sap_packet(header, deletion ? origin_line(entry.sdp) : entry.sdp, packet_);
    if (!length) {
        return statusbar::forward_failure(length);
    }
    if (::send(fd_.get(), packet_.data(), *length, MSG_DONTWAIT) < 0) {
        return statusbar::failure(statusbar::net::errno_code());
    }
    return statusbar::success();
}

}  // namespace padl::sap
