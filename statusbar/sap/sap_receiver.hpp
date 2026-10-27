#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RFC 2974 SAP, the listening side: SapDirectory, the table of announced sessions (pure: the
/// caller gives the time), and SapReceiver, the net::Pollable that joins the SAP groups and feeds
/// it, for receivers that subscribe to a session by its name (s=).

#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/sap/sap.hpp"
#include "statusbar/sdp/sdp.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::sap {

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
    sdp::Session const& session, ip::IPv4Address const& sap_origin, std::string_view name, std::string_view origin) -> bool;

class SapDirectory
{
  public:
    /// Ten AES67 announcement intervals (300 s), short of RFC 2974 §3.1's hour so a source that is
    /// gone shows as gone within minutes; a slower announcer gets ten of its own intervals, up to
    /// MAX_TIMEOUT_NS, and one heard only once twice the timeout.
    static constexpr int64_t DEFAULT_TIMEOUT_NS = 10 * SapAnnouncer::INTERVAL_NS;
    /// RFC 2974's hour: the most ten of a sender's intervals stretch the timeout to.
    static constexpr int64_t MAX_TIMEOUT_NS = 3'600'000'000'000;
    /// The most sessions kept; announcements of more are ignored.
    static constexpr size_t MAX_SESSIONS = 1024;

    enum class Kind : uint8_t
    {
        announced,  ///< new, or changed
        deleted,    ///< a SAP deletion
        timed_out,  ///< not re-announced within the timeout
    };

    struct Entry
    {
        SessionKey key;
        sdp::Session session;
        ip::IPv4Address sap_origin;
        uint16_t message_id_hash{0};
        int64_t last_seen_ns{0};
        int64_t interval_ns{0};  ///< the longer of its last two announcement gaps (0: announced once)
        int64_t last_gap_ns{0};
    };

    struct Event
    {
        Kind kind{Kind::announced};
        Entry entry;  ///< for deleted and timed_out, as it was last announced
    };

    explicit SapDirectory(int64_t const timeout_ns = DEFAULT_TIMEOUT_NS) noexcept
        : timeout_ns_{timeout_ns}
    {}

    /// A parsed SAP packet at `now_ns`: the event it makes, if any.
    [[nodiscard]] auto handle(SapMessage const& message, int64_t now_ns) -> std::optional<Event>;
    /// parse_sap_packet() and handle().
    [[nodiscard]] auto handle_packet(std::span<uint8_t const> packet, int64_t now_ns) -> std::optional<Event>;
    /// The sessions not re-announced within the timeout at `now_ns`, removed.
    [[nodiscard]] auto expire(int64_t now_ns) -> std::vector<Event>;

    /// The first session a subscription to `name` (and `origin`, when not
    /// empty) matches (subscription_matches()), or nullptr.
    [[nodiscard]] auto find(std::string_view name, std::string_view origin) const -> Entry const*;
    [[nodiscard]] auto entries() const noexcept -> std::span<Entry const> { return entries_; }
    [[nodiscard]] auto timeout_ns() const noexcept -> int64_t { return timeout_ns_; }

  private:
    [[nodiscard]] auto entry_timeout_ns(Entry const& entry) const noexcept -> int64_t;
    /// The entry a deletion names, or end(): by its o= line when it carries one that reads, else by message id hash.
    [[nodiscard]] auto deleted_entry(SapMessage const& message) -> std::vector<Entry>::iterator;
    /// A deletion: the entry it names, removed.
    [[nodiscard]] auto handle_deletion(SapMessage const& message) -> std::optional<Event>;
    /// An announcement: a new entry, or a changed one; nothing for a copy, a late version or a full directory.
    [[nodiscard]] auto handle_announcement(SapMessage const& message, int64_t now_ns) -> std::optional<Event>;
    /// A new session, announced; nothing once the directory holds MAX_SESSIONS.
    [[nodiscard]] auto add_entry(SessionKey key, sdp::Session session, SapMessage const& message, int64_t now_ns)
        -> std::optional<Event>;

    int64_t timeout_ns_;
    std::vector<Entry> entries_;
};

class SapReceiver final : public net::Pollable
{
  public:
    /// The SAP groups (RFC 2974 §3): administratively scoped (AES67's,
    /// first), organisation-local scope, and global scope.
    static constexpr std::array<ip::IPv4Address, 3> GROUPS{{
        SapAnnouncer::GROUP,
        ip::IPv4Address{239, 195, 255, 255},
        ip::IPv4Address{224, 2, 127, 254},
    }};

    using Handler = std::function<void(SapDirectory::Event const&)>;

    /// Listen on `port` for the SAP groups joined on `interface_index` (0:
    /// the kernel's choice). The first group must join; the others are best
    /// effort. With no groups (tests), it hears unicast to `port` only.
    [[nodiscard]] static auto open(
        unsigned int interface_index,
        std::span<ip::IPv4Address const> groups = GROUPS,
        uint16_t port = SapAnnouncer::PORT,
        int64_t timeout_ns = SapDirectory::DEFAULT_TIMEOUT_NS) -> StatusValue<std::unique_ptr<SapReceiver>>;

    SapReceiver(net::FileDescriptor fd, int64_t timeout_ns) noexcept;

    /// Where events go (nothing: they are dropped, the directory still kept).
    /// The handler runs inside on_ready() and tick(): it may read
    /// directory(), but must not destroy the receiver or replace the handler.
    void set_handler(Handler handler) { handler_ = std::move(handler); }
    [[nodiscard]] auto directory() const noexcept -> SapDirectory const& { return directory_; }

    [[nodiscard]] auto fd() const noexcept -> int override { return fd_.get(); }
    void on_ready(int64_t now_ns) override;
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::FileDescriptor fd_;
    SapDirectory directory_;
    Handler handler_;
    int64_t next_expiry_ns_{0};
};

}  // namespace statusbar::sap
