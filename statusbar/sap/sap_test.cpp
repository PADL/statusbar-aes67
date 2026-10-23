// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/sap/sap.hpp"

#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::sap;
using statusbar::ip::IPv4Address;

namespace {

auto session(std::string const& name) -> sdp::Session
{
    sdp::Session s{};
    s.session_id = 42;
    s.session_version = 1;
    s.origin_address = "127.0.0.1";
    s.name = name;
    s.connection = sdp::Connection{.address = IPv4Address{239, 69, 1, 2}, .ttl = 32};
    sdp::Media media{};
    media.port = 5004;
    media.payload_type = 96;
    media.channels = 2;
    media.media_clock = sdp::MediaClock{.direct_offset = 0};
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
        (void)::bind(fd_.get(), reinterpret_cast<sockaddr const*>(&address), sizeof(address));
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
    std::string const text = sdp::to_string(session("A"));
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
    EXPECT_TRUE(sdp::parse(message->sdp).has_value());

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
    EXPECT_EQ(sdp::parse(message->sdp)->name, std::string{"A"});

    sap.tick(t0 + SapAnnouncer::INTERVAL_NS - 1);  // not yet
    EXPECT_TRUE(listener.quiet());
    sap.tick(t0 + SapAnnouncer::INTERVAL_NS);
    auto const second = listener.next();
    EXPECT_TRUE(second.has_value());
    EXPECT_EQ(parse_sap_packet(*second)->message_id_hash, message->message_id_hash);

    // The same session again changes nothing; a changed one is sent at once,
    // after a deletion of the old, with a new hash.
    sap.announce("aes67-out-0", session("A"));
    sap.tick(t0 + SapAnnouncer::INTERVAL_NS + 1);
    EXPECT_TRUE(listener.quiet());
    sap.announce("aes67-out-0", session("B"));
    auto const deleted = listener.next();
    EXPECT_TRUE(deleted.has_value());
    EXPECT_TRUE(parse_sap_packet(*deleted)->deletion);
    EXPECT_EQ(parse_sap_packet(*deleted)->message_id_hash, message->message_id_hash);
    sap.tick(t0 + SapAnnouncer::INTERVAL_NS + 2);
    auto const changed = listener.next();
    EXPECT_TRUE(changed.has_value());
    EXPECT_NE(parse_sap_packet(*changed)->message_id_hash, message->message_id_hash);
    EXPECT_EQ(sdp::parse(parse_sap_packet(*changed)->sdp)->name, std::string{"B"});

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

TEST_MAIN(statusbar_sap, sap_test)
