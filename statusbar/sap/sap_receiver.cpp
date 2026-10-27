// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/sap/sap_receiver.hpp"

#include "statusbar/net/net_posix_util.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::sap {

namespace {

constexpr int64_t EXPIRY_PERIOD_NS = 1'000'000'000;
/// Copies of one announcement closer together than this are one announcement.
constexpr int64_t DUPLICATE_NS = 1'000'000'000;

/// Takes the next line from `text`, without its CR LF or LF.
[[nodiscard]] auto take_line(std::string_view& text) noexcept -> std::string_view
{
    auto const end = text.find('\n');
    auto line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    if (line.ends_with('\r')) {
        line.remove_suffix(1);
    }
    return line;
}

/// An absent payload type, or the SDP MIME type in any case (MIME types are case-insensitive).
[[nodiscard]] auto is_sdp_payload_type(std::string_view const payload_type) noexcept -> bool
{
    return payload_type.empty() || std::ranges::equal(payload_type, SAP_PAYLOAD_TYPE, [](char const a, char const b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

/// Whether a deletion names `entry`: from the origin that announced it, by its o= line and not older
/// than what was announced, or without one by message id hash (RFC 2974 Section 5; 0 names nothing).
[[nodiscard]] auto deletion_names(
    SapDirectory::Entry const& entry, SapMessage const& message, std::optional<OriginLine> const& line) noexcept -> bool
{
    if (entry.sap_origin != message.origin) {
        return false;
    }
    if (line) {
        return entry.key == line->key && line->version >= entry.session.session_version;
    }
    return message.message_id_hash != 0 && entry.message_id_hash == message.message_id_hash;
}

/// Records an announcement heard `gap` after the last; a copy (another group, another path) is not an interval.
void note_heard(SapDirectory::Entry& entry, SapMessage const& message, int64_t const gap, int64_t const now_ns) noexcept
{
    if (gap >= DUPLICATE_NS) {
        entry.interval_ns = std::max(gap, entry.last_gap_ns);  // the longer of the last two: one late burst passes
        entry.last_gap_ns = gap;
    }
    entry.last_seen_ns = now_ns;
    entry.message_id_hash = message.message_id_hash;
}

/// A re-announcement of a known entry: an event when its description changed. Only from the origin that
/// announced it; an older version is a late copy, unless it comes after the copies' window (a reboot).
[[nodiscard]] auto update_entry(SapDirectory::Entry& entry, SapMessage const& message, sdp::Session session, int64_t const now_ns)
    -> std::optional<SapDirectory::Event>
{
    int64_t const gap = now_ns - entry.last_seen_ns;
    if (message.origin != entry.sap_origin || (session.session_version < entry.session.session_version && gap < DUPLICATE_NS)) {
        return std::nullopt;
    }
    note_heard(entry, message, gap, now_ns);
    if (entry.session == session) {
        return std::nullopt;  // a re-announcement
    }
    entry.session = std::move(session);
    return SapDirectory::Event{.kind = SapDirectory::Kind::announced, .entry = entry};
}

/// Sets SO_REUSEADDR and binds `fd` to `port` on every address.
[[nodiscard]] auto bind_shared(int const fd, uint16_t const port) noexcept -> Status
{
    if (auto reuse = net::set_reuse_addr(fd); !reuse) {
        return reuse;
    }
    sockaddr_in address{};  // sin_addr zero: INADDR_ANY
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::bind(fd, net::sockaddr_cast(address), sizeof(address)) != 0) {
        return failure(net::errno_code());
    }
    return success();
}

/// Joins `groups` on the interface; only the first must succeed.
[[nodiscard]] auto join_groups(
    int const fd, std::span<ip::IPv4Address const> const groups, unsigned int const interface_index) noexcept -> Status
{
#if defined(IP_MULTICAST_ALL)
    // Only the groups joined here, not every group another socket on the host joined.
    int const all = 0;
    (void)::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &all, sizeof(all));
#endif
    for (size_t i = 0; i < groups.size(); ++i) {
        if (auto joined = net::join_group(fd, groups[i], interface_index); !joined && i == 0) {
            return failure(joined.error());
        }
    }
    return success();
}

}  // namespace

auto session_key(sdp::Session const& session) -> SessionKey
{
    return {.username = session.username, .session_id = session.session_id, .address = session.origin_address};
}

auto origin_line_of(std::string_view sdp) -> std::optional<OriginLine>
{
    while (!sdp.empty()) {
        auto const line = take_line(sdp);
        if (!line.starts_with("o=")) {
            continue;
        }
        sdp::Session session;
        if (!sdp::parse_origin_line(line.substr(2), session)) {
            return std::nullopt;
        }
        return OriginLine{.key = session_key(session), .version = session.session_version};
    }
    return std::nullopt;
}

auto subscription_matches(
    sdp::Session const& session, ip::IPv4Address const& sap_origin, std::string_view const name, std::string_view const origin)
    -> bool
{
    if (session.name != name) {
        return false;
    }
    if (origin.empty()) {
        return true;
    }
    if (session.origin_address == origin) {
        return true;
    }
    auto const pinned = ip::parse_ipv4(origin);
    return pinned && *pinned == sap_origin;
}

auto SapDirectory::handle(SapMessage const& message, int64_t const now_ns) -> std::optional<Event>
{
    if (!is_sdp_payload_type(message.payload_type)) {
        return std::nullopt;
    }
    return message.deletion ? handle_deletion(message) : handle_announcement(message, now_ns);
}

auto SapDirectory::deleted_entry(SapMessage const& message) -> std::vector<Entry>::iterator
{
    auto const line = origin_line_of(message.sdp);
    if (!line && (message.sdp.starts_with("o=") || message.sdp.find("\no=") != std::string_view::npos)) {
        return entries_.end();  // an o= line that does not read names nothing
    }
    return std::ranges::find_if(entries_, [&](Entry const& entry) { return deletion_names(entry, message, line); });
}

auto SapDirectory::handle_deletion(SapMessage const& message) -> std::optional<Event>
{
    auto const it = deleted_entry(message);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    Event event{.kind = Kind::deleted, .entry = std::move(*it)};
    entries_.erase(it);
    return event;
}

auto SapDirectory::handle_announcement(SapMessage const& message, int64_t const now_ns) -> std::optional<Event>
{
    auto session = sdp::parse(message.sdp);
    if (!session) {
        return std::nullopt;
    }
    auto key = session_key(*session);
    if (auto const it = std::ranges::find(entries_, key, &Entry::key); it != entries_.end()) {
        return update_entry(*it, message, std::move(*session), now_ns);
    }
    return add_entry(std::move(key), std::move(*session), message, now_ns);
}

auto SapDirectory::add_entry(SessionKey key, sdp::Session session, SapMessage const& message, int64_t const now_ns)
    -> std::optional<Event>
{
    if (entries_.size() >= MAX_SESSIONS) {
        return std::nullopt;
    }
    entries_.push_back(Entry{
        .key = std::move(key),
        .session = std::move(session),
        .sap_origin = message.origin,
        .message_id_hash = message.message_id_hash,
        .last_seen_ns = now_ns});
    return Event{.kind = Kind::announced, .entry = entries_.back()};
}

auto SapDirectory::handle_packet(std::span<uint8_t const> const packet, int64_t const now_ns) -> std::optional<Event>
{
    auto const message = parse_sap_packet(packet);
    return message ? handle(*message, now_ns) : std::nullopt;
}

auto SapDirectory::entry_timeout_ns(Entry const& entry) const noexcept -> int64_t
{
    if (entry.interval_ns == 0) {
        return 2 * timeout_ns_;  // heard once: its interval unknown (an RFC 2974 sender's is 300 s or more)
    }
    return std::clamp(10 * entry.interval_ns, timeout_ns_, std::max(timeout_ns_, MAX_TIMEOUT_NS));
}

auto SapDirectory::expire(int64_t const now_ns) -> std::vector<Event>
{
    std::vector<Event> events;
    std::erase_if(entries_, [&](Entry& entry) {
        if (now_ns - entry.last_seen_ns <= entry_timeout_ns(entry)) {
            return false;
        }
        events.push_back(Event{.kind = Kind::timed_out, .entry = std::move(entry)});
        return true;
    });
    return events;
}

auto SapDirectory::find(std::string_view const name, std::string_view const origin) const -> Entry const*
{
    auto const it = std::ranges::find_if(
        entries_, [&](Entry const& entry) { return subscription_matches(entry.session, entry.sap_origin, name, origin); });
    return it == entries_.end() ? nullptr : &*it;
}

auto SapReceiver::open(
    unsigned int const interface_index,
    std::span<ip::IPv4Address const> const groups,
    uint16_t const port,
    int64_t const timeout_ns) -> StatusValue<std::unique_ptr<SapReceiver>>
{
    auto fd = net::make_udp_socket();
    if (!fd) {
        return failure(fd.error());
    }
    if (auto bound = bind_shared(fd->get(), port); !bound) {
        return failure(bound.error());
    }
    if (auto joined = join_groups(fd->get(), groups, interface_index); !joined) {
        return failure(joined.error());
    }
    return std::make_unique<SapReceiver>(std::move(*fd), timeout_ns);
}

SapReceiver::SapReceiver(net::FileDescriptor fd, int64_t const timeout_ns) noexcept
    : fd_{std::move(fd)}
    , directory_{timeout_ns}
{}

void SapReceiver::on_ready(int64_t const now_ns)
{
    // A bounded batch per call: a flood must not hold the reactor.
    static constexpr int MAX_BATCH = 64;
    thread_local std::vector<uint8_t> buffer(65536);
    for (int i = 0; i < MAX_BATCH; ++i) {
        auto const n = ::recv(fd_.get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (n < 0) {
            if (net::was_interrupted()) {
                continue;
            }
            return;  // EAGAIN, or an error (recv clears it)
        }
        auto event = directory_.handle_packet(std::span{buffer}.first(static_cast<size_t>(n)), now_ns);
        if (event && handler_) {
            handler_(*event);
        }
    }
}

void SapReceiver::tick(int64_t const now_ns)
{
    if (now_ns < next_expiry_ns_) {
        return;
    }
    next_expiry_ns_ = now_ns + EXPIRY_PERIOD_NS;
    for (auto const& event : directory_.expire(now_ns)) {
        if (handler_) {
            handler_(event);
        }
    }
}

}  // namespace statusbar::sap
