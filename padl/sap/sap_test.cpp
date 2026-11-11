// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/sap/sap.hpp"

#include "padl/sap/sap_receiver.hpp"
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace padl::sap;
using statusbar::ip::IPv4Address;

namespace {

auto session(std::string const& name) -> padl::sdp::Session
{
    padl::sdp::Session s{};
    s.session_id = 42;
    s.session_version = 1;
    s.origin_address = "127.0.0.1";
    s.name = name;
    s.connection = padl::sdp::Connection{.address = IPv4Address{239, 69, 1, 2}, .ttl = 32};
    padl::sdp::Media media{};
    media.port = 5004;
    media.payload_type = 96;
    media.channels = 2;
    media.media_clock = padl::sdp::MediaClock{.direct_offset = 0};
    s.media.push_back(media);
    return s;
}

/// A socket bound to 127.0.0.1:port, and what arrives on it.
class Listener
{
  public:
    explicit Listener(uint16_t const port)
    {
        fd_ = net::FileDescriptor{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd_.get() >= 0) {
            (void)::bind(fd_.get(), net::sockaddr_cast(address), sizeof(address));
        }
    }

    auto next() -> std::optional<std::vector<uint8_t>>
    {
        for (int tries = 0; tries < 100; ++tries) {
            std::array<uint8_t, 2048> buffer{};
            auto const n = ::recv(fd_.get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (n > 0) {
                return std::vector<uint8_t>(buffer.begin(), buffer.begin() + n);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return std::nullopt;
    }

    /// The source address of the next datagram.
    auto next_source() -> std::optional<IPv4Address>
    {
        for (int tries = 0; tries < 100; ++tries) {
            std::array<uint8_t, 2048> buffer{};
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            if (::recvfrom(fd_.get(), buffer.data(), buffer.size(), MSG_DONTWAIT, net::sockaddr_cast(from), &length) > 0) {
                return IPv4Address{ntohl(from.sin_addr.s_addr)};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return std::nullopt;
    }

    auto quiet() -> bool
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        std::array<uint8_t, 2048> buffer{};
        return ::recv(fd_.get(), buffer.data(), buffer.size(), MSG_DONTWAIT) < 0;
    }

  private:
    net::FileDescriptor fd_;
};

}  // namespace

TEST(sap, packet_round_trips)
{
    std::string const text = padl::sdp::to_string(session("A"));
    auto const packet = make_sap_packet(false, 0xBEEF, IPv4Address{192, 168, 1, 10}, text);
    EXPECT_EQ(packet[0], uint8_t{0x20});
    EXPECT_EQ(packet[1], uint8_t{0});
    auto const message = parse_sap_packet(packet);
    EXPECT_TRUE(message.has_value());
    EXPECT_FALSE(message->deletion);
    EXPECT_EQ(message->message_id_hash, uint16_t{0xBEEF});
    EXPECT_TRUE(message->origin == (IPv4Address{192, 168, 1, 10}));
    EXPECT_EQ(message->payload_type, std::string{"application/sdp"});
    EXPECT_EQ(std::string{message->sdp}, text);
    EXPECT_TRUE(padl::sdp::parse(message->sdp).has_value());

    auto const deletion = make_sap_packet(true, 1, IPv4Address{10, 0, 0, 1}, "o=- 1 1 IN IP4 10.0.0.1\r\n");
    EXPECT_EQ(deletion[0], uint8_t{0x24});
    EXPECT_TRUE(parse_sap_packet(deletion)->deletion);

    auto bad = packet;
    bad[0] = 0x30;  // IPv6 origin flag
    EXPECT_FALSE(parse_sap_packet(bad).has_value());
    EXPECT_FALSE(parse_sap_packet(std::span{packet}.first(7)).has_value());
}

TEST(sap, announces_on_schedule_and_deletes_on_withdraw)
{
    Listener listener{49875};
    auto announcer = SapAnnouncer::open(0, IPv4Address{127, 0, 0, 1}, IPv4Address{127, 0, 0, 1}, 49875);
    EXPECT_TRUE(announcer.has_value());
    auto& sap = **announcer;
    EXPECT_EQ(sap.fd(), -1);

    sap.announce("aes67-out-0", session("A"));
    EXPECT_EQ(sap.announced(), size_t{1});
    int64_t const t0 = 1'000'000'000;
    sap.tick(t0);
    auto const first = listener.next();
    EXPECT_TRUE(first.has_value());
    auto const message = parse_sap_packet(*first);
    EXPECT_TRUE(message.has_value());
    EXPECT_FALSE(message->deletion);
    EXPECT_EQ(padl::sdp::parse(message->sdp)->name, std::string{"A"});

    sap.tick(t0 + SapAnnouncer::INTERVAL_NS - 1);  // not yet
    EXPECT_TRUE(listener.quiet());
    sap.tick(t0 + SapAnnouncer::INTERVAL_NS);
    auto const second = listener.next();
    EXPECT_TRUE(second.has_value());
    EXPECT_EQ(parse_sap_packet(*second)->message_id_hash, message->message_id_hash);

    // The same session again changes nothing; a changed one is a single announcement at the next tick, with a new
    // hash and no deletion before it (RFC 2974 Section 5).
    sap.announce("aes67-out-0", session("A"));
    sap.tick(t0 + SapAnnouncer::INTERVAL_NS + 1);
    EXPECT_TRUE(listener.quiet());
    sap.announce("aes67-out-0", session("B"));
    EXPECT_TRUE(listener.quiet());
    sap.tick(t0 + SapAnnouncer::INTERVAL_NS + 2);
    auto const changed = listener.next();
    auto const update = changed ? parse_sap_packet(*changed) : std::nullopt;
    EXPECT_TRUE(update.has_value());
    if (!update) {
        return;
    }
    EXPECT_FALSE(update->deletion);
    EXPECT_NE(update->message_id_hash, message->message_id_hash);
    EXPECT_EQ(padl::sdp::parse(update->sdp)->name, std::string{"B"});
    EXPECT_TRUE(listener.quiet());

    EXPECT_TRUE(sap.withdraw("aes67-out-0").has_value());
    EXPECT_EQ(sap.announced(), size_t{0});
    auto const gone = listener.next();
    EXPECT_TRUE(gone.has_value());
    auto const goodbye = parse_sap_packet(*gone);
    EXPECT_TRUE(goodbye->deletion);
    EXPECT_TRUE(std::string{goodbye->sdp}.starts_with("o=- 42 1 IN IP4 127.0.0.1"));
    sap.tick(t0 + (3 * SapAnnouncer::INTERVAL_NS));
    EXPECT_TRUE(listener.quiet());
    EXPECT_FALSE(sap.finished());
}

TEST(sap, withdraw_all_sends_every_deletion_at_once)
{
    // As a daemon shutting down: every announced session's deletion goes out
    // before withdraw_all() returns, with no tick after it.
    Listener listener{49876};
    auto announcer = SapAnnouncer::open(0, IPv4Address{127, 0, 0, 1}, IPv4Address{127, 0, 0, 1}, 49876);
    EXPECT_TRUE(announcer.has_value());
    auto& sap = **announcer;
    sap.announce("aes67-out-0", session("A"));
    sap.announce("aes67-out-1", session("B"));
    sap.tick(1'000'000'000);
    std::vector<uint16_t> hashes;
    for (int i = 0; i < 2; ++i) {
        auto const packet = listener.next();
        EXPECT_TRUE(packet.has_value());
        hashes.push_back(parse_sap_packet(*packet)->message_id_hash);
    }

    auto const sent = sap.withdraw_all();
    EXPECT_TRUE(sent.has_value());
    EXPECT_EQ(*sent, size_t{2});
    EXPECT_EQ(sap.announced(), size_t{0});
    for (int i = 0; i < 2; ++i) {
        auto const packet = listener.next();
        EXPECT_TRUE(packet.has_value());
        auto const message = parse_sap_packet(*packet);
        EXPECT_TRUE(message->deletion);
        EXPECT_TRUE(std::ranges::find(hashes, message->message_id_hash) != hashes.end());
    }
    EXPECT_TRUE(listener.quiet());
    EXPECT_EQ(*sap.withdraw_all(), size_t{0});  // nothing left
    EXPECT_TRUE(sap.withdraw("aes67-out-0").has_value());
}

namespace {

auto dante_session(std::string const& name, uint64_t const version, std::string const& origin = "10.10.32.236")
    -> padl::sdp::Session
{
    auto s = session(name);
    s.session_id = 1234;
    s.session_version = version;
    s.origin_address = origin;
    return s;
}

/// The port a socket is bound to.
auto port_of(int const fd) -> uint16_t
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    return ntohs(address.sin_port);
}

/// `packet` fed to `directory` as its origin field's host sends it.
auto from_origin(SapDirectory& directory, std::span<uint8_t const> const packet, int64_t const now_ns)
    -> std::optional<SapDirectory::Event>
{
    auto const message = parse_sap_packet(packet);
    return directory.handle_packet(packet, message ? message->origin : IPv4Address{}, now_ns);
}

auto announcement(padl::sdp::Session const& s, IPv4Address const& origin = IPv4Address{10, 10, 32, 236}, uint16_t const hash = 7)
    -> std::vector<uint8_t>
{
    return make_sap_packet(false, hash, origin, padl::sdp::to_string(s));
}

}  // namespace

TEST(sap_directory, announce_refresh_newer_version_delete)
{
    SapDirectory directory;
    auto const first = from_origin(directory, announcement(dante_session("AVIOUSBC-1 : 1", 1)), 0);
    EXPECT_TRUE(first.has_value() && first->kind == SapDirectory::Kind::announced);
    EXPECT_TRUE(first && first->entry.session.name == "AVIOUSBC-1 : 1");
    EXPECT_TRUE(first && first->entry.sap_origin == (IPv4Address{10, 10, 32, 236}));
    EXPECT_EQ(directory.entries().size(), size_t{1});

    // the same again only refreshes
    EXPECT_FALSE(from_origin(directory, announcement(dante_session("AVIOUSBC-1 : 1", 1)), 1).has_value());
    // a new version is announced again, the same session (key) kept
    auto const newer = from_origin(directory, announcement(dante_session("AVIOUSBC-1 : 1", 2), IPv4Address{10, 10, 32, 236}, 8), 2);
    EXPECT_TRUE(newer.has_value() && newer->kind == SapDirectory::Kind::announced);
    EXPECT_TRUE(newer && newer->entry.session.session_version == 2);
    EXPECT_EQ(directory.entries().size(), size_t{1});

    // another device's session of the same name is another session
    EXPECT_TRUE(from_origin(directory, announcement(dante_session("AVIOUSBC-1 : 1", 1, "10.10.32.248")), 3).has_value());
    EXPECT_EQ(directory.entries().size(), size_t{2});

    // a deletion carrying only the o= line (as SapAnnouncer sends)
    auto const deletion = make_sap_packet(true, 8, IPv4Address{10, 10, 32, 236}, "o=- 1234 2 IN IP4 10.10.32.236\r\n");
    auto const deleted = from_origin(directory, deletion, 4);
    EXPECT_TRUE(deleted.has_value() && deleted->kind == SapDirectory::Kind::deleted);
    EXPECT_TRUE(deleted && deleted->entry.session.name == "AVIOUSBC-1 : 1");
    EXPECT_TRUE(deleted && deleted->entry.session.origin_address == "10.10.32.236");
    EXPECT_EQ(directory.entries().size(), size_t{1});
    EXPECT_FALSE(from_origin(directory, deletion, 5).has_value());  // already gone
}

TEST(sap_directory, a_stale_or_foreign_deletion_is_ignored)
{
    SapDirectory directory;
    (void)from_origin(directory, announcement(dante_session("A", 2)), 0);
    // a late copy of the deletion of version 1, which the sender sent before announcing 2
    EXPECT_FALSE(
        from_origin(directory, make_sap_packet(true, 7, IPv4Address{10, 10, 32, 236}, "o=- 1234 1 IN IP4 10.10.32.236\r\n"), 1)
            .has_value());
    // from another origin
    EXPECT_FALSE(
        from_origin(directory, make_sap_packet(true, 7, IPv4Address{10, 10, 32, 9}, "o=- 1234 2 IN IP4 10.10.32.236\r\n"), 1)
            .has_value());
    EXPECT_EQ(directory.entries().size(), size_t{1});
    // without a payload type: the o= line straight after the header (RFC 2974: optional)
    std::vector<uint8_t> bare{0x24, 0, 0, 7, 10, 10, 32, 236};
    std::string const line = "o=- 1234 2 IN IP4 10.10.32.236\r\n";
    bare.insert(bare.end(), line.begin(), line.end());
    auto const deleted = from_origin(directory, bare, 2);
    EXPECT_TRUE(deleted.has_value() && deleted->kind == SapDirectory::Kind::deleted);
    EXPECT_TRUE(directory.entries().empty());

    // a hash of 0 names nothing
    (void)from_origin(directory, announcement(dante_session("B", 1), IPv4Address{10, 10, 32, 236}, 0), 3);
    EXPECT_FALSE(from_origin(directory, make_sap_packet(true, 0, IPv4Address{10, 10, 32, 236}, ""), 4).has_value());
    EXPECT_EQ(directory.entries().size(), size_t{1});
}

TEST(sap_directory, a_reannouncement_from_another_origin_is_ignored)
{
    SapDirectory directory;
    (void)from_origin(directory, announcement(dante_session("A", 2)), 0);
    EXPECT_FALSE(from_origin(directory, announcement(dante_session("A", 3), IPv4Address{10, 10, 32, 9}), 1).has_value());
    EXPECT_EQ(directory.entries().front().session.session_version, uint64_t{2});
    EXPECT_TRUE(directory.entries().front().sap_origin == (IPv4Address{10, 10, 32, 236}));

    // the payload type in another case
    std::string const text = padl::sdp::to_string(dante_session("B", 1, "10.0.0.2"));
    std::vector<uint8_t> packet{0x20, 0, 0, 1, 10, 0, 0, 2};
    std::string const type = "Application/SDP";
    packet.insert(packet.end(), type.begin(), type.end());
    packet.push_back(0);
    packet.insert(packet.end(), text.begin(), text.end());
    EXPECT_TRUE(from_origin(directory, packet, 2).has_value());
    // a deletion whose o= line does not read names nothing, whatever its hash
    EXPECT_FALSE(
        from_origin(directory, make_sap_packet(true, 1, IPv4Address{10, 0, 0, 2}, "o=- x 1 IN IP4 10.0.0.2\r\n"), 3).has_value());
    EXPECT_EQ(directory.entries().size(), size_t{2});
}

TEST(sap_directory, deletion_by_origin_and_hash)
{
    SapDirectory directory;
    (void)from_origin(directory, announcement(dante_session("A", 1), IPv4Address{10, 0, 0, 1}, 0x1234), 0);
    auto const deleted = from_origin(directory, make_sap_packet(true, 0x1234, IPv4Address{10, 0, 0, 1}, ""), 1);
    EXPECT_TRUE(deleted.has_value() && deleted->kind == SapDirectory::Kind::deleted);
    EXPECT_TRUE(directory.entries().empty());
}

TEST(sap_directory, times_out_sessions_not_reannounced)
{
    int64_t const timeout = 10 * SapAnnouncer::INTERVAL_NS;
    SapDirectory directory{timeout};
    EXPECT_TRUE(SapDirectory::DEFAULT_TIMEOUT_NS > 2 * (300'000'000'000 + 100'000'000'000));  // RFC 2974's slowest
    (void)from_origin(directory, announcement(dante_session("A", 1)), 0);
    (void)from_origin(directory, announcement(dante_session("B", 1, "10.0.0.2"), IPv4Address{10, 0, 0, 2}), 0);
    (void)from_origin(directory, announcement(dante_session("B", 1, "10.0.0.2"), IPv4Address{10, 0, 0, 2}), timeout / 20);
    EXPECT_TRUE(directory.expire(timeout).empty());
    auto const expired = directory.expire(timeout + 1);
    EXPECT_EQ(expired.size(), size_t{1});
    for (auto const& event : expired) {
        EXPECT_TRUE(event.kind == SapDirectory::Kind::timed_out);
        EXPECT_EQ(event.entry.session.name, std::string{"A"});
    }
    EXPECT_EQ(directory.entries().size(), size_t{1});
    EXPECT_TRUE(directory.expire(timeout + (timeout / 20)).empty());
    EXPECT_EQ(directory.expire(timeout + (timeout / 20) + 1).size(), size_t{1});
    EXPECT_TRUE(directory.entries().empty());
}

TEST(sap_directory, a_restarted_version_counter_is_the_session_now)
{
    SapDirectory directory;
    (void)from_origin(directory, announcement(dante_session("A", 9)), 0);
    // Version 1 after version 9, changed: a sender that restarted its counter (a reboot), read as the session now.
    auto changed = dante_session("A", 1);
    changed.information = "rebooted";
    auto const rebooted = from_origin(directory, announcement(changed, IPv4Address{10, 10, 32, 236}, 8), 1);
    EXPECT_TRUE(rebooted.has_value() && rebooted->entry.session.session_version == 1);
    EXPECT_EQ(directory.entries().size(), size_t{1});
}

TEST(sap_directory, ignores_compressed_encrypted_and_other_payloads)
{
    SapDirectory directory;
    auto packet = announcement(dante_session("A", 1));
    auto compressed = packet;
    compressed[0] |= 0x01U;
    auto encrypted = packet;
    encrypted[0] |= 0x02U;
    EXPECT_FALSE(from_origin(directory, compressed, 0).has_value());
    EXPECT_FALSE(from_origin(directory, encrypted, 0).has_value());

    // another payload type
    std::string const text = padl::sdp::to_string(dante_session("A", 1));
    std::vector<uint8_t> other{0x20, 0, 0, 1, 10, 0, 0, 1};
    std::string const type = "application/x-other";
    other.insert(other.end(), type.begin(), type.end());
    other.push_back(0);
    other.insert(other.end(), text.begin(), text.end());
    EXPECT_FALSE(from_origin(directory, other, 0).has_value());
    // SDP the parser refuses
    EXPECT_FALSE(from_origin(directory, make_sap_packet(false, 1, IPv4Address{10, 0, 0, 1}, "v=0\r\nbogus\r\n"), 0).has_value());
    EXPECT_TRUE(directory.entries().empty());

    // no payload type at all is SDP (RFC 2974: optional)
    std::vector<uint8_t> bare{0x20, 0, 0, 1, 10, 0, 0, 1};
    bare.insert(bare.end(), text.begin(), text.end());
    EXPECT_TRUE(from_origin(directory, bare, 0).has_value());
    // authentication data is skipped
    std::vector<uint8_t> authenticated{0x20, 1, 0, 2, 10, 0, 0, 2, 0xDE, 0xAD, 0xBE, 0xEF};
    std::string const text2 = padl::sdp::to_string(dante_session("B", 1, "10.0.0.2"));
    authenticated.insert(authenticated.end(), SAP_PAYLOAD_TYPE.begin(), SAP_PAYLOAD_TYPE.end());
    authenticated.push_back(0);
    authenticated.insert(authenticated.end(), text2.begin(), text2.end());
    EXPECT_TRUE(from_origin(directory, authenticated, 0).has_value());
    EXPECT_EQ(directory.entries().size(), size_t{2});
}

TEST(sap_directory, finds_by_name_and_origin)
{
    SapDirectory directory;
    (void)from_origin(directory, announcement(dante_session("AVIOUSBC-1 : 1", 1, "10.10.32.248"), IPv4Address{10, 10, 32, 248}), 0);
    (void)from_origin(directory, announcement(dante_session("AVIOUSBC-1 : 1", 1, "10.10.32.236"), IPv4Address{10, 10, 32, 236}), 0);
    // a host name in o= is matched by the SAP origin
    (void)from_origin(directory, announcement(dante_session("Named", 1, "avio.local"), IPv4Address{10, 10, 32, 9}), 0);
    auto const* any = directory.find("AVIOUSBC-1 : 1", "");
    EXPECT_TRUE(any != nullptr && any->session.origin_address == "10.10.32.248");
    auto const* pinned = directory.find("AVIOUSBC-1 : 1", "10.10.32.236");
    EXPECT_TRUE(pinned != nullptr && pinned->session.origin_address == "10.10.32.236");
    EXPECT_TRUE(directory.find("AVIOUSBC-1 : 1", "10.10.32.1") == nullptr);
    EXPECT_TRUE(directory.find("AVIOUSBC-1 : 2", "") == nullptr);
    EXPECT_TRUE(directory.find("Named", "10.10.32.9") != nullptr);
    EXPECT_TRUE(directory.find("Named", "avio.local") != nullptr);

    auto const line = origin_line_of("v=0\r\no=jdoe 2890844526 2890842807 IN IP4 10.47.16.5\r\ns=x\r\n");
    EXPECT_TRUE(
        line.has_value() && line->key.username == "jdoe" && line->key.session_id == 2890844526ULL &&
        line->key.address == "10.47.16.5" && line->version == 2890842807ULL);
    EXPECT_FALSE(origin_line_of("o=jdoe x 1 IN IP4 10.47.16.5\n").has_value());
    EXPECT_FALSE(origin_line_of("s=nothing\n").has_value());
}

TEST(sap_directory, a_forged_origin_announcement_or_deletion_is_ignored)
{
    SapDirectory directory;
    IPv4Address const origin{10, 10, 32, 236};
    IPv4Address const forger{10, 10, 32, 9};
    EXPECT_FALSE(directory.handle_packet(announcement(dante_session("A", 1), origin), forger, 0).has_value());
    EXPECT_TRUE(directory.entries().empty());
    EXPECT_TRUE(directory.handle_packet(announcement(dante_session("A", 1), origin), origin, 1).has_value());
    auto const deletion = make_sap_packet(true, 7, origin, "o=- 1234 1 IN IP4 10.10.32.236\r\n");
    EXPECT_FALSE(directory.handle_packet(deletion, forger, 2).has_value());
    EXPECT_EQ(directory.entries().size(), size_t{1});
    auto const changed = announcement(dante_session("A", 2), origin, 8);
    EXPECT_FALSE(directory.handle_packet(changed, forger, 3).has_value());
    EXPECT_EQ(directory.entries().front().session.session_version, uint64_t{1});
    EXPECT_TRUE(directory.handle_packet(deletion, origin, 4).has_value());
}

TEST(sap_directory, a_packet_longer_than_max_packet_is_ignored)
{
    SapDirectory directory;
    auto s = dante_session("Long", 1);
    auto const fits = announcement(s);
    s.information = std::string(SapDirectory::MAX_PACKET + 1 - fits.size() + 1, 'x');  // one octet over, after "i="
    auto const long_packet = announcement(s);
    EXPECT_TRUE(long_packet.size() > SapDirectory::MAX_PACKET);
    EXPECT_FALSE(from_origin(directory, long_packet, 0).has_value());
    s.information.resize(s.information.size() - (long_packet.size() - SapDirectory::MAX_PACKET));
    auto const at_limit = announcement(s);
    EXPECT_EQ(at_limit.size(), SapDirectory::MAX_PACKET);
    EXPECT_TRUE(from_origin(directory, at_limit, 1).has_value());
}

namespace {

/// An announcement of session `id` from origin 10.10.<host / 256>.<host % 256>.
auto numbered(uint64_t const id, uint32_t const host) -> std::vector<uint8_t>
{
    IPv4Address const origin{10, 10, static_cast<uint8_t>(host / 256), static_cast<uint8_t>(host % 256)};
    auto s = dante_session("S" + std::to_string(id), 1, std::format("10.10.{}.{}", host / 256, host % 256));
    s.session_id = id;
    return announcement(s, origin);
}

}  // namespace

TEST(sap_directory, a_full_directory_evicts_the_session_announced_least_recently)
{
    SapDirectory directory;
    size_t const origins = SapDirectory::MAX_SESSIONS / SapDirectory::MAX_SESSIONS_PER_ORIGIN;
    for (uint64_t i = 0; i < SapDirectory::MAX_SESSIONS; ++i) {
        (void)from_origin(directory, numbered(i, static_cast<uint32_t>(i % origins)), static_cast<int64_t>(i));
    }
    EXPECT_EQ(directory.entries().size(), SapDirectory::MAX_SESSIONS);
    (void)from_origin(directory, numbered(0, 0), 5000);  // S0 re-announced: S1 is now the least recent
    auto const event = from_origin(directory, numbered(9999, 200), 5001);
    EXPECT_TRUE(event.has_value());
    if (!event) {
        return;
    }
    EXPECT_TRUE(event->kind == SapDirectory::Kind::announced);
    EXPECT_EQ(event->evicted.value_or(SapDirectory::Entry{}).session.name, std::string{"S1"});
    EXPECT_EQ(directory.entries().size(), SapDirectory::MAX_SESSIONS);
    EXPECT_TRUE(directory.find("S9999", "") != nullptr);
    EXPECT_TRUE(directory.find("S0", "") != nullptr);
    EXPECT_TRUE(directory.find("S1", "") == nullptr);
}

TEST(sap_directory, one_origin_past_its_share_evicts_its_own_sessions)
{
    SapDirectory directory;
    (void)from_origin(directory, numbered(1, 1), 0);  // another origin's, the least recent of all
    for (uint64_t i = 0; i < SapDirectory::MAX_SESSIONS_PER_ORIGIN; ++i) {
        (void)from_origin(directory, numbered(100 + i, 7), static_cast<int64_t>(i + 1));
    }
    auto const event = from_origin(directory, numbered(9999, 7), 5000);
    EXPECT_TRUE(event.has_value());
    if (!event) {
        return;
    }
    EXPECT_EQ(event->evicted.value_or(SapDirectory::Entry{}).session.name, std::string{"S100"});
    EXPECT_EQ(directory.entries().size(), SapDirectory::MAX_SESSIONS_PER_ORIGIN + 1);
    EXPECT_TRUE(directory.find("S1", "") != nullptr);
}

TEST(sap_receiver, receives_announcements_and_deletions)
{
    auto receiver = SapReceiver::open(0, {}, 0);
    EXPECT_TRUE(receiver.has_value());
    if (!receiver) {
        return;
    }
    auto& sap = **receiver;
    EXPECT_TRUE(sap.fd() >= 0);
    std::vector<SapDirectory::Event> events;
    sap.set_handler([&events](SapDirectory::Event const& event) { events.push_back(event); });
    auto announcer = SapAnnouncer::open(0, IPv4Address{127, 0, 0, 1}, IPv4Address{127, 0, 0, 1}, port_of(sap.fd()));
    EXPECT_TRUE(announcer.has_value());
    if (!announcer) {
        return;
    }
    auto const pump = [&](size_t const want) {
        for (int tries = 0; tries < 200 && events.size() < want; ++tries) {
            sap.on_ready(0);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    };
    (*announcer)->announce("out", session("Loop"));
    (*announcer)->tick(0);
    pump(1);
    EXPECT_EQ(events.size(), size_t{1});
    EXPECT_TRUE(!events.empty() && events.back().kind == SapDirectory::Kind::announced);
    EXPECT_TRUE(sap.directory().find("Loop", "127.0.0.1") != nullptr);
    EXPECT_TRUE((*announcer)->withdraw("out").has_value());
    pump(2);
    EXPECT_EQ(events.size(), size_t{2});
    EXPECT_TRUE(events.size() == 2 && events.back().kind == SapDirectory::Kind::deleted);
    EXPECT_TRUE(sap.directory().entries().empty());
    EXPECT_FALSE(sap.finished());
}

TEST(sap_receiver, a_forged_long_or_malformed_datagram_is_dropped_and_counted)
{
    auto receiver = SapReceiver::open(0, {}, 0);
    EXPECT_TRUE(receiver.has_value());
    if (!receiver) {
        return;
    }
    auto& sap = **receiver;
    std::vector<SapDirectory::Event> events;
    sap.set_handler([&events](SapDirectory::Event const& event) { events.push_back(event); });
    // Sent from 127.0.0.1, naming another host as its origin.
    auto const packet = make_sap_packet(false, 1, IPv4Address{10, 0, 0, 9}, padl::sdp::to_string(session("Forged")));
    net::FileDescriptor const sender{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port_of(sap.fd()));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_TRUE(::sendto(sender.get(), packet.data(), packet.size(), 0, net::sockaddr_cast(to), sizeof(to)) > 0);
    // A packet longer than MAX_PACKET from its origin (cut at MAX_PACKET + 1 on reading), and one that is not SAP.
    auto const long_packet = make_sap_packet(false, 2, IPv4Address{127, 0, 0, 1}, std::string(SapDirectory::MAX_PACKET, 'x'));
    std::array<uint8_t, 3> const junk{0x20, 0, 0};
    for (auto const datagram : {std::span<uint8_t const>{long_packet}, std::span<uint8_t const>{junk}}) {
        EXPECT_TRUE(::sendto(sender.get(), datagram.data(), datagram.size(), 0, net::sockaddr_cast(to), sizeof(to)) > 0);
    }
    for (int tries = 0; tries < 200 && sap.dropped() < 3; ++tries) {
        sap.on_ready(0);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(sap.directory().entries().empty());
    EXPECT_EQ(sap.dropped(), uint64_t{3});  // refusals are counted, so a forged origin can be seen
}

TEST(sap, an_announcer_sends_from_its_origin)
{
    // Bound to its origin, 127.0.0.2: a host with several addresses sends from the one its packets name.
    Listener listener{49877};
    auto announcer = SapAnnouncer::open(0, IPv4Address{127, 0, 0, 2}, IPv4Address{127, 0, 0, 1}, 49877);
    EXPECT_TRUE(announcer.has_value());
    if (!announcer) {
        return;
    }
    (*announcer)->announce("out", session("From"));
    (*announcer)->tick(0);
    auto const from = listener.next_source();
    EXPECT_TRUE(from == (std::optional{IPv4Address{127, 0, 0, 2}}));
}

TEST(sap_receiver, expires_on_tick)
{
    auto receiver = SapReceiver::open(0, {}, 0, 1000);
    EXPECT_TRUE(receiver.has_value());
    if (!receiver) {
        return;
    }
    auto& sap = **receiver;
    std::vector<SapDirectory::Event> events;
    sap.set_handler([&events](SapDirectory::Event const& event) { events.push_back(event); });
    auto announcer = SapAnnouncer::open(0, IPv4Address{127, 0, 0, 1}, IPv4Address{127, 0, 0, 1}, port_of(sap.fd()));
    EXPECT_TRUE(announcer.has_value());
    if (!announcer) {
        return;
    }
    (*announcer)->announce("out", session("Short"));
    (*announcer)->tick(0);
    for (int tries = 0; tries < 200 && events.empty(); ++tries) {
        sap.on_ready(5'000);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(events.size(), size_t{1});
    sap.tick(5'500);  // within the timeout
    EXPECT_EQ(events.size(), size_t{1});
    sap.tick(2'000'000'000);
    EXPECT_EQ(events.size(), size_t{2});
    EXPECT_TRUE(events.size() == 2 && events.back().kind == SapDirectory::Kind::timed_out);
}

TEST_MAIN(padl_sap, sap_test)
