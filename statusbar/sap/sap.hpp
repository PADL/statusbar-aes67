#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RFC 2974 SAP: packets, and announcements of AES67 senders' sessions as
/// Dante and other AES67 devices discover streams: each session is announced to
/// 239.255.255.255:9875 (the administratively scoped SAP group) on the first
/// tick and every 30 s after, and a deletion is sent when it is withdrawn.
///
/// A net::Pollable for the reactor thread; it only sends, so fd() is -1 and
/// the work is in tick(). announce() and withdraw() are called on the same
/// thread.

#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/sdp/sdp.hpp"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::sap {

/// The SAP payload type field: SDP.
inline constexpr std::string_view SAP_PAYLOAD_TYPE = "application/sdp";

/// A SAP packet (RFC 2974 section 3): version 1, IPv4 origin, no
/// authentication or encryption, `deletion` setting the message type, then
/// the payload type and the SDP text.
[[nodiscard]] auto make_sap_packet(bool deletion, uint16_t message_id_hash, ip::IPv4Address const& origin, std::string_view sdp)
    -> std::vector<uint8_t>;

/// What a SAP packet says, for tests and listeners: nullopt unless it is a
/// well-formed version 1 IPv4 packet with no encryption or compression.
struct SapMessage
{
    bool deletion{false};
    uint16_t message_id_hash{0};
    ip::IPv4Address origin;
    std::string payload_type;  ///< empty when absent
    std::string_view sdp;      ///< a view into the packet
};
[[nodiscard]] auto parse_sap_packet(std::span<uint8_t const> packet) -> std::optional<SapMessage>;

class SapAnnouncer final : public net::Pollable
{
  public:
    static constexpr int64_t INTERVAL_NS = 30'000'000'000;
    /// The administratively scoped SAP group (RFC 2974 section 3)
    static constexpr ip::IPv4Address GROUP{239, 255, 255, 255};
    static constexpr uint16_t PORT = 9875;

    /// Announce out of `interface` (by index; 0 lets the kernel choose) from
    /// `origin`, to `destination`:`port` (the SAP group, or another for tests).
    [[nodiscard]] static auto open(
        unsigned int interface_index,
        ip::IPv4Address const& origin,
        ip::IPv4Address const& destination = GROUP,
        uint16_t port = PORT,
        uint8_t ttl = 255) -> StatusValue<std::unique_ptr<SapAnnouncer>>;

    SapAnnouncer(net::FileDescriptor fd, ip::IPv4Address const& origin) noexcept;

    /// Announce `session` under `key` (a slot id, say), replacing what was
    /// announced under it; the next tick sends it. A changed session gets a new
    /// message id hash, so listeners take it as new.
    void announce(std::string const& key, sdp::Session const& session);
    /// Stop announcing `key`, sending its deletion now.
    void withdraw(std::string const& key);

    [[nodiscard]] auto announced() const noexcept -> size_t { return entries_.size(); }

    [[nodiscard]] auto fd() const noexcept -> int override { return -1; }
    void on_ready(int64_t /*now_ns*/) override {}
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    struct Entry
    {
        std::string key;
        std::string sdp;
        uint16_t hash{0};
        int64_t next_ns{0};
        bool due{true};
    };

    void send(bool deletion, Entry const& entry);

    net::FileDescriptor fd_;
    ip::IPv4Address origin_;
    std::vector<Entry> entries_;
};

}  // namespace statusbar::sap
