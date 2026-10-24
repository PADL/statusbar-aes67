// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/sap/sap.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::sap {

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

auto hash_of(std::string_view const sdp) noexcept -> uint16_t
{
    auto const h = std::hash<std::string_view>{}(sdp);
    auto value = static_cast<uint16_t>(h ^ (h >> 16U) ^ (h >> 32U) ^ (h >> 48U));
    return value == 0 ? uint16_t{1} : value;  // 0 means "no hash"
}

}  // namespace

auto make_sap_packet(bool const deletion, uint16_t const message_id_hash, ip::IPv4Address const& origin, std::string_view const sdp)
    -> std::vector<uint8_t>
{
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
    if (packet.size() < 8) {
        return std::nullopt;
    }
    uint8_t const flags = packet[0];
    if ((flags & 0xE0U) != SAP_V1 || (flags & (SAP_IPV6 | SAP_ENCRYPTED | SAP_COMPRESSED)) != 0) {
        return std::nullopt;
    }
    size_t const auth = size_t{packet[1]} * 4;
    if (packet.size() < 8 + auth) {
        return std::nullopt;
    }
    SapMessage message{};
    message.deletion = (flags & SAP_DELETION) != 0;
    message.message_id_hash = static_cast<uint16_t>((packet[2] << 8U) | packet[3]);
    message.origin = ip::IPv4Address{packet[4], packet[5], packet[6], packet[7]};
    std::string_view rest{reinterpret_cast<char const*>(packet.data()) + 8 + auth, packet.size() - 8 - auth};
    // The payload type is optional: SDP text starts "v=0" (a deletion's may
    // be only its "o=" line, or nothing).
    if (!rest.empty() && !rest.starts_with("v=") && !rest.starts_with("o=")) {
        auto const nul = rest.find('\0');
        if (nul == std::string_view::npos) {
            return std::nullopt;
        }
        message.payload_type = std::string{rest.substr(0, nul)};
        rest.remove_prefix(nul + 1);
    }
    message.sdp = rest;
    return message;
}

auto SapAnnouncer::open(
    unsigned int const interface_index,
    ip::IPv4Address const& origin,
    ip::IPv4Address const& destination,
    uint16_t const port,
    uint8_t const ttl) -> StatusValue<std::unique_ptr<SapAnnouncer>>
{
    auto fd = net::make_udp_socket();
    if (!fd) {
        return failure(fd.error());
    }
    if (destination.is_multicast()) {
        if (auto set = net::set_multicast_send(fd->get(), interface_index, ttl, true); !set) {
            return failure(set.error());
        }
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(destination.to_uint32());
    if (::connect(fd->get(), reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0) {
        return failure(net::errno_code());
    }
    return std::make_unique<SapAnnouncer>(std::move(*fd), origin);
}

SapAnnouncer::SapAnnouncer(net::FileDescriptor fd, ip::IPv4Address const& origin) noexcept
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
        (void)send(true, *it);  // the old description goes before the new one comes
        it->sdp = std::move(text);
        it->hash = hash_of(it->sdp);
        it->due = true;
        return;
    }
    Entry entry{.key = key, .sdp = std::move(text)};
    entry.hash = hash_of(entry.sdp);
    entries_.push_back(std::move(entry));
}

auto SapAnnouncer::withdraw(std::string const& key) -> Status
{
    auto const it = std::ranges::find(entries_, key, &Entry::key);
    if (it == entries_.end()) {
        return success();
    }
    auto status = send(true, *it);
    entries_.erase(it);
    return status;
}

auto SapAnnouncer::withdraw_all() -> StatusValue<size_t>
{
    auto const entries = std::exchange(entries_, {});
    size_t sent = 0;
    Status first = success();
    for (auto const& entry : entries) {
        if (auto status = send(true, entry); status) {
            ++sent;
        } else if (first) {
            first = status;
        }
    }
    if (!first) {
        return failure(first.error());
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

auto SapAnnouncer::send(bool const deletion, Entry const& entry) -> Status
{
    auto const packet = make_sap_packet(deletion, entry.hash, origin_, deletion ? origin_line(entry.sdp) : entry.sdp);
    if (::send(fd_.get(), packet.data(), packet.size(), MSG_DONTWAIT) < 0) {
        return failure(net::errno_code());
    }
    return success();
}

}  // namespace statusbar::sap
