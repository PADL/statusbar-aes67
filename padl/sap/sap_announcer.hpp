#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RFC 2974 SAP, the announcing side: SapAnnouncer, announcing AES67 senders' sessions to
/// 239.255.255.255:9875 every 30 s with a deletion on withdraw. A net::Pollable that only sends
/// (fd() is -1, the work is in tick()); announce() and withdraw() run on the reactor thread.

#include "padl/sap/sap_packet.hpp"
#include "padl/sdp/sdp_session.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace padl::sap {

class SapAnnouncer final : public statusbar::net::Pollable
{
  public:
    static constexpr int64_t INTERVAL_NS = 30'000'000'000;
    /// The administratively scoped SAP group (RFC 2974 Section 3)
    static constexpr statusbar::ip::IPv4Address GROUP{239, 255, 255, 255};
    static constexpr uint16_t PORT = 9875;

    /// Announce out of `interface` (by index; 0 lets the kernel choose) from `origin`, an address of this host the
    /// socket is bound to, to `destination`:`port` (the SAP group, or another for tests).
    [[nodiscard]] static auto open(
        unsigned int interface_index,
        statusbar::ip::IPv4Address const& origin,
        statusbar::ip::IPv4Address const& destination = GROUP,
        uint16_t port = PORT,
        uint8_t ttl = 255) -> statusbar::StatusValue<std::unique_ptr<SapAnnouncer>>;

    SapAnnouncer(statusbar::net::FileDescriptor fd, statusbar::ip::IPv4Address const& origin) noexcept;

    /// Announce `session` under `key` (a slot id, say), replacing what was announced under it; the next tick sends
    /// it. A changed session goes under a new message id hash, with no deletion first (RFC 2974 Section 5).
    /// Fails message_size, changing nothing, when its packet would be longer than MAX_SAP_PACKET.
    [[nodiscard]] auto announce(std::string const& key, sdp::Session const& session) -> statusbar::Status;
    /// Stop announcing `key`, sending its deletion now, not at the next tick (a caller shutting
    /// down may close straight after): the send's error, or success (also for a key not announced).
    [[nodiscard]] auto withdraw(std::string const& key) -> statusbar::Status;
    /// withdraw() every announced key: how many deletions went out, or the
    /// first send error (every key is withdrawn either way).
    [[nodiscard]] auto withdraw_all() -> statusbar::StatusValue<size_t>;

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

    /// `text` stored under `key`: a new entry, or a changed one due at the next tick.
    void store(std::string const& key, std::string text);
    /// `entry`'s announcement, or its deletion (its o= line alone): fails when it does not fit MAX_SAP_PACKET.
    [[nodiscard]] auto send(bool deletion, Entry const& entry) -> statusbar::Status;

    statusbar::net::FileDescriptor fd_;
    statusbar::ip::IPv4Address origin_;
    std::vector<Entry> entries_;
    std::array<uint8_t, MAX_SAP_PACKET> packet_{};
};

}  // namespace padl::sap
