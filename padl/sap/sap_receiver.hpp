#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RFC 2974 SAP, the listening side: SapDirectory, the table of announced sessions (pure: the
/// caller gives the time), and SapReceiver, the net::Pollable that joins the SAP groups and feeds
/// it, for receivers that subscribe to a session by its name (s=).

#include "padl/sap/sap.hpp"
#include "padl/sdp/sdp.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace padl::sap {

/// A session's identity: its SDP origin's username, session id and address
/// (RFC 8866 §5.2), which stay the same across versions.
struct SessionKey
{
    std::string username;
    uint64_t session_id{0};
    std::string address;

    auto operator==(SessionKey const&) const noexcept -> bool = default;
};

[[nodiscard]] auto session_key(sdp::Session const& session) -> SessionKey;

/// What an SDP text's o= line says (all a deletion need carry).
struct OriginLine
{
    SessionKey key;
    uint64_t version{0};
};

/// The o= line of an SDP text, read as sdp::parse() reads it, or nullopt if
/// it has none that reads.
[[nodiscard]] auto origin_line_of(std::string_view sdp) -> std::optional<OriginLine>;

/// Whether `session`, announced from SAP origin `sap_origin`, is the one a
/// subscription to session name `name` means: its s= is `name`, and, when
/// `origin` is not empty, its o= address or its SAP origin is `origin`.
[[nodiscard]] auto subscription_matches(
    sdp::Session const& session, statusbar::ip::IPv4Address const& sap_origin, std::string_view name, std::string_view origin)
    -> bool;

class SapDirectory
{
  public:
    /// 15 minutes: two of RFC 2974 Section 3.1's slowest-starting intervals (300 s plus a third), short of its hour,
    /// so a source that is gone shows as gone within minutes.
    static constexpr int64_t DEFAULT_TIMEOUT_NS = 900'000'000'000;
    /// The most sessions kept; a new one past them evicts the one announced least recently.
    static constexpr size_t MAX_SESSIONS = 1024;
    /// The most sessions one origin keeps; a new one past them evicts that origin's least recent.
    static constexpr size_t MAX_SESSIONS_PER_ORIGIN = 128;
    /// The largest SAP packet read: room for a large ST 2110-30 or ST 2022-7 description, sent fragmented (RFC 2974
    /// Section 6 only recommends under 1 KB); Session::MAX_MEDIA bounds what its SDP parses to.
    static constexpr size_t MAX_PACKET = 4096;

    enum class Kind : uint8_t
    {
        announced,  ///< new, or changed
        deleted,    ///< a SAP deletion
        timed_out,  ///< not re-announced within the timeout
        evicted,    ///< removed from a full directory for a new session
    };

    struct Entry
    {
        SessionKey key;
        sdp::Session session;
        statusbar::ip::IPv4Address sap_origin;
        uint16_t message_id_hash{0};
        int64_t last_seen_ns{0};
    };

    struct Event
    {
        Kind kind{Kind::announced};
        Entry entry;                   ///< for deleted, timed_out and evicted, as it was last announced
        std::optional<Entry> evicted;  ///< announced into a full directory: the entry it replaced (Kind::evicted)
    };

    explicit SapDirectory(int64_t const timeout_ns = DEFAULT_TIMEOUT_NS) noexcept
        : timeout_ns_{timeout_ns}
    {}

    /// A parsed SAP packet at `now_ns`: the event it makes, if any. Trusts `message.origin`.
    [[nodiscard]] auto handle(SapMessage const& message, int64_t now_ns) -> std::optional<Event>;
    /// The SAP message in `packet` from `source`, its IP source address, or nullopt: longer than MAX_PACKET, not a
    /// packet parse_sap_packet() reads, or an origin field naming another host (RFC 2974 Section 6).
    [[nodiscard]] static auto admit(std::span<uint8_t const> packet, statusbar::ip::IPv4Address const& source)
        -> std::optional<SapMessage>;
    /// admit() and handle(): nothing for a packet admit() refuses.
    [[nodiscard]] auto handle_packet(std::span<uint8_t const> packet, statusbar::ip::IPv4Address const& source, int64_t now_ns)
        -> std::optional<Event>;
    /// The sessions not re-announced within the timeout at `now_ns`, removed.
    [[nodiscard]] auto expire(int64_t now_ns) -> std::vector<Event>;

    /// The first session a subscription to `name` (and `origin`, when not
    /// empty) matches (subscription_matches()), or nullptr.
    [[nodiscard]] auto find(std::string_view name, std::string_view origin) const -> Entry const*;
    [[nodiscard]] auto entries() const noexcept -> std::span<Entry const> { return entries_; }
    [[nodiscard]] auto timeout_ns() const noexcept -> int64_t { return timeout_ns_; }

  private:
    /// The entry a deletion names, or end(): by its o= line when it carries one that reads, else by message id hash.
    [[nodiscard]] auto deleted_entry(SapMessage const& message) -> std::vector<Entry>::iterator;
    /// A deletion: the entry it names, removed.
    [[nodiscard]] auto handle_deletion(SapMessage const& message) -> std::optional<Event>;
    /// An announcement: a new entry, or a changed one; nothing for a copy or another origin's.
    [[nodiscard]] auto handle_announcement(SapMessage const& message, int64_t now_ns) -> std::optional<Event>;
    /// A new session, announced, with the entry make_room() evicted for it.
    [[nodiscard]] auto add_entry(SessionKey key, sdp::Session session, SapMessage const& message, int64_t now_ns) -> Event;
    /// Room for a session from `origin`: the least recent of its own sessions once it holds MAX_SESSIONS_PER_ORIGIN,
    /// else of all once the directory is full, removed; nullopt when there is room.
    [[nodiscard]] auto make_room(statusbar::ip::IPv4Address const& origin) -> std::optional<Entry>;
    /// The entry at `it`, removed.
    [[nodiscard]] auto remove_entry(std::vector<Entry>::iterator it) -> Entry;

    int64_t timeout_ns_;
    std::vector<Entry> entries_;
};

class SapReceiver final : public statusbar::net::Pollable
{
  public:
    /// The SAP groups (RFC 2974 §3): administratively scoped (AES67's,
    /// first), organisation-local scope, and global scope.
    static constexpr std::array<statusbar::ip::IPv4Address, 3> GROUPS{{
        SapAnnouncer::GROUP,
        statusbar::ip::IPv4Address{239, 195, 255, 255},
        statusbar::ip::IPv4Address{224, 2, 127, 254},
    }};

    using Handler = std::function<void(SapDirectory::Event const&)>;

    /// Listen on `port` for the SAP groups joined on `interface_index` (0:
    /// the kernel's choice). The first group must join; the others are best
    /// effort. With no groups (tests), it receives unicast to `port` only.
    [[nodiscard]] static auto open(
        unsigned int interface_index,
        std::span<statusbar::ip::IPv4Address const> groups = GROUPS,
        uint16_t port = SapAnnouncer::PORT,
        int64_t timeout_ns = SapDirectory::DEFAULT_TIMEOUT_NS) -> statusbar::StatusValue<std::unique_ptr<SapReceiver>>;

    SapReceiver(statusbar::net::FileDescriptor fd, int64_t timeout_ns) noexcept;

    /// Where events go (nothing: they are dropped, the directory still kept); an eviction goes as its own event
    /// before the announcement that caused it. The handler runs inside on_ready() and tick(): it may read
    /// directory(), but must not destroy the receiver or replace the handler.
    void set_handler(Handler handler) { handler_ = std::move(handler); }
    [[nodiscard]] auto directory() const noexcept -> SapDirectory const& { return directory_; }
    /// Datagrams SapDirectory::admit() refused: too long, not SAP, or from another host than their origin. Any thread.
    [[nodiscard]] auto dropped() const noexcept -> uint64_t { return dropped_.load(std::memory_order_relaxed); }

    [[nodiscard]] auto fd() const noexcept -> int override { return fd_.get(); }
    void on_ready(int64_t now_ns) override;
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    /// Reads one datagram into the directory; false when none is waiting.
    [[nodiscard]] auto receive_one(int64_t now_ns) -> bool;
    /// The handler's events for `event`, if any: its eviction, then itself.
    void deliver(std::optional<SapDirectory::Event> const& event);

    statusbar::net::FileDescriptor fd_;
    SapDirectory directory_;
    std::array<uint8_t, SapDirectory::MAX_PACKET + 1> buffer_{};  ///< one octet over: a longer packet reads as too long
    std::atomic<uint64_t> dropped_{0};                            ///< dropped(): datagrams admit() refused
    Handler handler_;
    int64_t next_expiry_ns_{0};
};

}  // namespace padl::sap
