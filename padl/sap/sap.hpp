#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RFC 2974 SAP: packets, and SapAnnouncer, announcing AES67 senders' sessions to
/// 239.255.255.255:9875 every 30 s with a deletion on withdraw. A net::Pollable that only sends
/// (fd() is -1, the work is in tick()); announce() and withdraw() run on the reactor thread.

#include "padl/sdp/sdp_session.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace padl::sap {

using statusbar::ieee::doublet_t;
using statusbar::ieee::octet_t;
using statusbar::ieee::quadlet_t;

/// The SAP payload type field: SDP.
inline constexpr std::string_view SAP_PAYLOAD_TYPE = "application/sdp";

/// The largest SAP packet sent or read: room for a large ST 2110-30 or ST 2022-7 description, sent fragmented
/// (RFC 2974 Section 6 only recommends under 1 KB).
inline constexpr size_t MAX_SAP_PACKET = 4096;

/// SAP header - RFC 2974 Section 3 (8 bytes, network order), with an IPv4 originating source
struct SapHeader
{
    /// Length of the header on the wire, before any authentication data
    static constexpr size_t LENGTH = 8;

    /// The only SAP version
    static constexpr uint8_t VERSION = 1;

    /// Byte 0: version[7:5] | address_type[4] | reserved[3] | message_type[2] | encrypted[1] | compressed[0]
    octet_t flags;

    /// Byte 1: authentication data length, in 32-bit words
    octet_t auth_length;

    /// Bytes 2-3: message identifier hash
    doublet_t message_id_hash;

    /// Bytes 4-7: originating source
    quadlet_t origin;

    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return flags.get_bits(0xE0U, 5); }
    [[nodiscard]] constexpr auto ipv6() const noexcept -> bool { return flags.has_flag(0x10U); }
    [[nodiscard]] constexpr auto deletion() const noexcept -> bool { return flags.has_flag(0x04U); }
    [[nodiscard]] constexpr auto encrypted() const noexcept -> bool { return flags.has_flag(0x02U); }
    [[nodiscard]] constexpr auto compressed() const noexcept -> bool { return flags.has_flag(0x01U); }

    /// A version 1 header with an IPv4 origin, no authentication, encryption or compression.
    constexpr void init(bool const is_deletion, uint16_t const hash, statusbar::ip::IPv4Address const& source) noexcept
    {
        flags = static_cast<uint8_t>(VERSION << 5U);
        flags.set_flag(0x04U, is_deletion);
        auth_length = 0U;
        message_id_hash = hash;
        origin = source.to_uint32();
    }

    auto operator<=>(SapHeader const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(SapHeader) == SapHeader::LENGTH, "SapHeader must be exactly 8 bytes");
static_assert(alignof(SapHeader) == 1, "SapHeader must be byte aligned");
static_assert(offsetof(SapHeader, flags) == 0, "flags must be at offset 0");
static_assert(offsetof(SapHeader, auth_length) == 1, "auth_length must be at offset 1");
static_assert(offsetof(SapHeader, message_id_hash) == 2, "message_id_hash must be at offset 2");
static_assert(offsetof(SapHeader, origin) == 4, "origin must be at offset 4");

/// Write a SAP packet into `datagram`: `header` with no authentication data (its length cleared), the payload type
/// and `sdp`. Returns the packet length, or the buffer's error when it does not fit.
[[nodiscard]] auto serialize_sap_packet(SapHeader const& header, std::string_view sdp, std::span<uint8_t> datagram) noexcept
    -> statusbar::StatusValue<size_t>;

/// What a SAP packet says, for tests and listeners: nullopt unless it is a
/// well-formed version 1 IPv4 packet with no encryption or compression.
struct SapMessage
{
    bool deletion{false};
    uint16_t message_id_hash{0};
    statusbar::ip::IPv4Address origin;
    std::string payload_type;  ///< empty when absent
    std::string_view sdp;      ///< a view into the packet
};
[[nodiscard]] auto parse_sap_packet(std::span<uint8_t const> packet) -> std::optional<SapMessage>;

/// The o= line of an SDP text, with its line ending, as a deletion carries it (RFC 2974 Section 3); empty for none.
[[nodiscard]] auto origin_line(std::string_view sdp) noexcept -> std::string_view;

class SapAnnouncer final : public statusbar::net::Pollable
{
  public:
    static constexpr int64_t INTERVAL_NS = 30'000'000'000;
    /// The administratively scoped SAP group (RFC 2974 §3)
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
    void announce(std::string const& key, sdp::Session const& session);
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

    /// `entry`'s announcement, or its deletion (its o= line alone): fails when it does not fit MAX_SAP_PACKET.
    [[nodiscard]] auto send(bool deletion, Entry const& entry) -> statusbar::Status;

    statusbar::net::FileDescriptor fd_;
    statusbar::ip::IPv4Address origin_;
    std::vector<Entry> entries_;
    std::array<uint8_t, MAX_SAP_PACKET> packet_{};
};

}  // namespace padl::sap

// Serialization traits - SapHeader is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<padl::sap::SapHeader> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace padl::sap {
using statusbar::protocol::load_unchecked;
using statusbar::protocol::store_unchecked;
}  // namespace padl::sap
