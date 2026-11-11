// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/sap/sap.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/net/net_posix_util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace padl::sap {

namespace {

constexpr uint8_t SAP_V1 = 0x20;          // V=1
constexpr uint8_t SAP_IPV6 = 0x10;        // A
constexpr uint8_t SAP_DELETION = 0x04;    // T
constexpr uint8_t SAP_ENCRYPTED = 0x02;   // E
constexpr uint8_t SAP_COMPRESSED = 0x01;  // C

/// The "o=" line of an SDP text, which a deletion carries (RFC 2974 §3).
auto origin_line(std::string_view const sdp) -> std::string_view
{
    auto const end = sdp.find('\n');
    auto line = sdp.substr(0, end == std::string_view::npos ? sdp.size() : end + 1);
    if (line.starts_with("v=")) {
        auto const rest = sdp.substr(line.size());
        auto const end2 = rest.find('\n');
        line = rest.substr(0, end2 == std::string_view::npos ? rest.size() : end2 + 1);
    }
    return line;
}

/// The message id hash of `sdp`, never `previous` (RFC 2974 Section 5: a modified session has a new one) nor 0,
/// which means "no hash".
auto hash_of(std::string_view const sdp, uint16_t const previous = 0) noexcept -> uint16_t
{
    auto const h = std::hash<std::string_view>{}(sdp);
    auto value = static_cast<uint16_t>(h ^ (h >> 16U) ^ (h >> 32U) ^ (h >> 48U));
    value = value == previous ? static_cast<uint16_t>(previous + 1) : value;
    return value == 0 ? uint16_t{1} : value;
}

/// A version 1 header with no IPv6 origin, encryption or compression, and room for its authentication data.
[[nodiscard]] auto is_supported_header(std::span<uint8_t const> const packet) noexcept -> bool
{
    if (packet.size() < 8) {
        return false;
    }
    uint8_t const flags = packet[0];
    return (flags & 0xE0U) == SAP_V1 && (flags & (SAP_IPV6 | SAP_ENCRYPTED | SAP_COMPRESSED)) == 0 &&
        packet.size() >= 8 + (size_t{packet[1]} * 4);
}

/// Parses the optional payload type from `rest`: SDP text starts "v=0" (a deletion's may be only its "o="
/// line, or nothing), anything else is a NUL-terminated type. False when the NUL is missing.
[[nodiscard]] auto parse_payload_type(std::string_view& rest, std::string& payload_type) -> bool
{
    if (rest.empty() || rest.starts_with("v=") || rest.starts_with("o=")) {
        return true;
    }
    auto const nul = rest.find('\0');
    if (nul == std::string_view::npos) {
        return false;
    }
    payload_type = std::string{rest.substr(0, nul)};
    rest.remove_prefix(nul + 1);
    return true;
}

/// An IPv4 socket address.
[[nodiscard]] auto socket_address(statusbar::ip::IPv4Address const& address, uint16_t const port) noexcept -> sockaddr_in
{
    sockaddr_in socket{};
    socket.sin_family = AF_INET;
    socket.sin_port = htons(port);
    socket.sin_addr.s_addr = htonl(address.to_uint32());
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

auto make_sap_packet(
    bool const deletion, uint16_t const message_id_hash, statusbar::ip::IPv4Address const& origin, std::string_view const sdp)
    -> std::vector<uint8_t>
{  // NOLINT(statusbar-block-statements): the RFC 2974 Section 3 header, field by field
    std::vector<uint8_t> packet;
    packet.reserve(8 + SAP_PAYLOAD_TYPE.size() + 1 + sdp.size());
    packet.push_back(static_cast<uint8_t>(SAP_V1 | (deletion ? SAP_DELETION : 0U)));
    packet.push_back(0);  // authentication length
    packet.push_back(static_cast<uint8_t>(message_id_hash >> 8U));
    packet.push_back(static_cast<uint8_t>(message_id_hash));
    for (size_t i = 0; i < 4; ++i) {
        packet.push_back(origin.octet(i));
    }
    packet.insert(packet.end(), SAP_PAYLOAD_TYPE.begin(), SAP_PAYLOAD_TYPE.end());
    packet.push_back(0);
    packet.insert(packet.end(), sdp.begin(), sdp.end());
    return packet;
}

auto parse_sap_packet(std::span<uint8_t const> const packet) -> std::optional<SapMessage>
{
    if (!is_supported_header(packet)) {
        return std::nullopt;
    }
    SapMessage message{
        .deletion = (packet[0] & SAP_DELETION) != 0,
        .message_id_hash = static_cast<uint16_t>((packet[2] << 8U) | packet[3]),
        .origin = statusbar::ip::IPv4Address{packet[4], packet[5], packet[6], packet[7]},
    };
    std::string_view rest = statusbar::as_string_view(packet.subspan(8 + (size_t{packet[1]} * 4)));
    if (!parse_payload_type(rest, message.payload_type)) {
        return std::nullopt;
    }
    message.sdp = rest;
    return message;
}

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
        it->hash = hash_of(it->sdp, it->hash);
        it->due = true;
        return;
    }
    Entry entry{.key = key, .sdp = std::move(text)};
    entry.hash = hash_of(entry.sdp);
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
    auto const packet = make_sap_packet(deletion, entry.hash, origin_, deletion ? origin_line(entry.sdp) : entry.sdp);
    if (::send(fd_.get(), packet.data(), packet.size(), MSG_DONTWAIT) < 0) {
        return statusbar::failure(statusbar::net::errno_code());
    }
    return statusbar::success();
}

}  // namespace padl::sap
