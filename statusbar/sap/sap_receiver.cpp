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

}  // namespace

auto session_key(sdp::Session const& session) -> SessionKey
{
    return {.username = session.username, .session_id = session.session_id, .address = session.origin_address};
}

auto origin_line_of(std::string_view sdp) -> std::optional<OriginLine>
{
    while (!sdp.empty()) {
        auto const end = sdp.find('\n');
        auto line = sdp.substr(0, end);
        sdp.remove_prefix(end == std::string_view::npos ? sdp.size() : end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
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
    // MIME types are case-insensitive.
    if (!message.payload_type.empty() &&
        !std::ranges::equal(message.payload_type, SAP_PAYLOAD_TYPE, [](char const a, char const b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        })) {
        return std::nullopt;
    }
    if (message.deletion) {
        // By the o= line it carries, from the origin that announced it, and not older than what
        // was announced; without one, by origin and message id hash (RFC 2974 §5; 0 names nothing).
        auto const line = origin_line_of(message.sdp);
        if (!line && (message.sdp.starts_with("o=") || message.sdp.find("\no=") != std::string_view::npos)) {
            return std::nullopt;  // an o= line that does not read names nothing
        }
        auto const it = std::ranges::find_if(entries_, [&](Entry const& e) {
            if (e.sap_origin != message.origin) {
                return false;
            }
            if (line) {
                return e.key == line->key && line->version >= e.session.session_version;
            }
            return message.message_id_hash != 0 && e.message_id_hash == message.message_id_hash;
        });
        if (it == entries_.end()) {
            return std::nullopt;
        }
        Event event{.kind = Kind::deleted, .entry = std::move(*it)};
        entries_.erase(it);
        return event;
    }
    auto session = sdp::parse(message.sdp);
    if (!session) {
        return std::nullopt;
    }
    auto key = session_key(*session);
    auto const it = std::ranges::find(entries_, key, &Entry::key);
    if (it != entries_.end()) {
        // Only from the origin that announced it (whose deletion counts). An
        // older version is a late copy, unless it comes after the copies'
        // window: a device whose version counter restarted (a reboot).
        int64_t const gap = now_ns - it->last_seen_ns;
        if (message.origin != it->sap_origin || (session->session_version < it->session.session_version && gap < DUPLICATE_NS)) {
            return std::nullopt;
        }
        if (gap >= DUPLICATE_NS) {                             // a copy (another group, another path) is not an interval
            it->interval_ns = std::max(gap, it->last_gap_ns);  // the longer of the last two: one late burst passes
            it->last_gap_ns = gap;
        }
        it->last_seen_ns = now_ns;
        it->message_id_hash = message.message_id_hash;
        if (it->session == *session) {
            return std::nullopt;  // a re-announcement
        }
        it->session = std::move(*session);
        return Event{.kind = Kind::announced, .entry = *it};
    }
    if (entries_.size() >= MAX_SESSIONS) {
        return std::nullopt;
    }
    entries_.push_back(Entry{
        .key = std::move(key),
        .session = std::move(*session),
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
    if (auto reuse = net::set_reuse_addr(fd->get()); !reuse) {
        return failure(reuse.error());
    }
#if defined(IP_MULTICAST_ALL)
    // Only the groups joined here, not every group another socket on the host joined.
    int const all = 0;
    (void)::setsockopt(fd->get(), IPPROTO_IP, IP_MULTICAST_ALL, &all, sizeof(all));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd->get(), net::sockaddr_cast(address), sizeof(address)) != 0) {
        return failure(net::errno_code());
    }
    for (size_t i = 0; i < groups.size(); ++i) {
        if (auto joined = net::join_group(fd->get(), groups[i], interface_index); !joined && i == 0) {
            return failure(joined.error());
        }
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
