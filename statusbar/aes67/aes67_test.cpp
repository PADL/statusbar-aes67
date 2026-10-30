// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/aes67/aes67_receiver.hpp"
#include "statusbar/aes67/aes67_sender.hpp"
#include "statusbar/aes67/aes67_socket.hpp"
#include "statusbar/aes67/aes67_udp_transport.hpp"
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/mediaclock/mediaclock_timebase.hpp"
#include "statusbar/rtp/rtp_header.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::aes67;
using statusbar::ip::IPv4Address;

namespace {

constexpr uint32_t FPP = 48;

/// A loopback address of this process's own, 127.(1-64).x.y from the pid: the suite runs in
/// several processes at once (ctest -j, one per file and one for all), and receivers bind their
/// destination address, so no other process's datagrams reach these tests' ports.
auto own_loopback() -> IPv4Address
{
    auto const pid = static_cast<uint32_t>(::getpid());
    return IPv4Address{
        127, static_cast<uint8_t>(1 + ((pid >> 16U) % 64)), static_cast<uint8_t>(pid >> 8U), static_cast<uint8_t>(pid)};
}

/// A multicast group of this process's own (239.(64-127).x.y from the pid), for the same reason.
auto own_group() -> IPv4Address
{
    auto const pid = static_cast<uint32_t>(::getpid());
    return IPv4Address{
        239, static_cast<uint8_t>(64 + ((pid >> 16U) % 64)), static_cast<uint8_t>(pid >> 8U), static_cast<uint8_t>(pid)};
}

Grandmaster const GRANDMASTER{.identity = *ieee::eui64_from_string("00-1D-C1-FF-FE-12-34-56"), .domain = 0};

constexpr StreamFormat STEREO_L24{.encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP};
constexpr StreamFormat STEREO_L32{.encoding = sdp::Encoding::l32, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP};

/// Byte `b` of frame `index`: a pattern a receiver can check by index.
auto pattern(uint64_t const index, size_t const b) -> uint8_t
{
    return static_cast<uint8_t>((index * 7) + b);
}

/// Generates the pattern for any index, or refuses indices in `missing`.
class PatternSource
{
  public:
    explicit PatternSource(uint32_t const frame_bytes)
        : frame_bytes_{frame_bytes}
    {}

    auto provider() -> FrameProvider
    {
        return [this](uint64_t const first_index, uint32_t const frames, std::span<uint8_t> const out) {
            return pull(first_index, frames, out) ? Supply::frames : Supply::missing;
        };
    }

    auto pull(uint64_t const first_index, uint32_t const frames, std::span<uint8_t> const out) noexcept -> bool
    {
        ++pulls;
        if (first_index == refuse_index) {
            return false;
        }
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t b = 0; b < frame_bytes_; ++b) {
                out[(size_t{f} * frame_bytes_) + b] = pattern(first_index + f, b);
            }
        }
        return true;
    }

    uint64_t refuse_index{UINT64_MAX};
    size_t pulls{0};

  private:
    uint32_t frame_bytes_;
};

struct Received
{
    uint64_t first_index;
    uint32_t frames;
    std::vector<uint8_t> bytes;
};

class RecordingSink
{
  public:
    auto handler() -> FrameHandler
    {
        return [this](uint64_t const first_index, uint32_t const frames, std::span<uint8_t const> const bytes) {
            blocks.push_back({first_index, frames, {bytes.begin(), bytes.end()}});
        };
    }
    std::vector<Received> blocks;
};

/// A transport in memory: what senders send, receivers receive.
class LoopTransport final : public Transport
{
  public:
    struct Packet
    {
        Endpoint destination;
        std::vector<uint8_t> bytes;
    };

    class Rx final : public ReceivePort
    {
      public:
        Rx(LoopTransport& t, Endpoint const& d)
            : transport{t}
            , destination{d}
        {}
        auto receive(std::span<Datagram> const out) noexcept -> size_t override
        {
            ++transport.receives;
            held.clear();
            size_t n = 0;
            while (n < out.size() && !transport.queue.empty()) {
                auto packet = std::move(transport.queue.front());
                transport.queue.pop_front();
                if (packet.destination != destination) {
                    continue;
                }
                held.push_back(std::move(packet.bytes));
                out[n++] = {.payload = held.back(), .source = transport.source};
            }
            return n;
        }
        LoopTransport& transport;
        Endpoint destination;
        std::deque<std::vector<uint8_t>> held;
    };

    class Tx final : public SendPort
    {
      public:
        Tx(LoopTransport& t, Endpoint const& d)
            : transport{t}
            , destination{d}
        {}
        auto send(rtp::RtpHeader const& header, std::span<uint8_t const> const payload) noexcept -> bool override
        {
            std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + payload.size());
            (void)rtp::serialize_rtp(header, payload, bytes);
            transport.queue.push_back({destination, std::move(bytes)});
            return true;
        }
        auto send_rendered(rtp::RtpHeader const& header, size_t const payload_length, PayloadRenderer const& render) noexcept
            -> bool override
        {
            if (transport.refuse) {
                return false;  // no TX buffer: render is not called
            }
            std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + payload_length);
            auto const offset = rtp::serialize_rtp_header(header, payload_length, bytes);
            if (!offset || !render(std::span{bytes}.subspan(*offset, payload_length))) {
                return false;
            }
            transport.queue.push_back({destination, std::move(bytes)});
            return true;
        }
        auto send_rendered_at(
            rtp::RtpHeader const& header,
            size_t const payload_length,
            PayloadRenderer const& render,
            uint64_t const departure_ns) noexcept -> bool override
        {
            transport.departures.push_back(departure_ns);
            return send_rendered(header, payload_length, render);
        }
        auto flush() noexcept -> size_t override { return 0; }
        LoopTransport& transport;
        Endpoint destination;
    };

    [[nodiscard]] auto local_address() const noexcept -> IPv4Address override { return source.address; }
    auto open_receive(Endpoint const& destination) -> StatusValue<std::unique_ptr<ReceivePort>> override
    {
        return std::make_unique<Rx>(*this, destination);
    }
    auto open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>> override
    {
        return std::make_unique<Tx>(*this, options.destination);
    }

    [[nodiscard]] auto send_error() const noexcept -> std::error_code override { return failing; }
    [[nodiscard]] auto drain_every_tick() const noexcept -> bool override { return every_tick; }

    std::deque<Packet> queue;
    std::vector<uint64_t> departures;  ///< each packet's departure as the sender gave it
    Endpoint source{.address = IPv4Address{192, 168, 1, 10}, .port = 5004};
    bool refuse{false};       ///< every send_rendered() finds no TX buffer
    std::error_code failing;  ///< send_error()
    bool every_tick{false};   ///< drain_every_tick()
    size_t receives{0};       ///< receive() calls
};

auto stream_session(Endpoint const& destination, std::optional<uint32_t> const offset = 0) -> sdp::Session
{
    sdp::Session session{};
    session.origin_address = "192.168.1.10";
    session.connection = sdp::Connection{.address = destination.address, .ttl = 32};
    sdp::Media media{};
    media.port = destination.port;
    media.payload_type = 97;
    media.encoding = sdp::Encoding::l24;
    media.channels = 2;
    if (offset) {
        media.media_clock = sdp::MediaClock{.direct_offset = *offset};
    }
    session.media.push_back(media);
    return session;
}

auto rtp_packet(uint16_t const seq, uint32_t const timestamp, uint32_t const ssrc = 0x1234, uint8_t const pt = 97)
    -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload(size_t{FPP} * 6);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = pattern(timestamp + (i / 6), i % 6);
    }
    rtp::RtpHeader header{};
    header.init(pt, seq, timestamp, ssrc);
    std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + payload.size());
    (void)rtp::serialize_rtp(header, payload, bytes);
    return bytes;
}

void expect_pattern(Received const& block)
{
    for (size_t i = 0; i < block.bytes.size(); ++i) {
        EXPECT_EQ(block.bytes[i], pattern(block.first_index + (i / 6), i % 6));
    }
}

}  // namespace

TEST(aes67_sender, announces_l32_that_an_l32_receiver_takes)
{
    LoopTransport transport;
    PatternSource source{8};
    Sender sender{STEREO_L32, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004, .payload_type = 98}));
    sender.set_grandmaster(GRANDMASTER);
    std::string const text = sdp::to_string(*sender.session());
    EXPECT_TRUE(text.find("m=audio 5004 RTP/AVP 98\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=rtpmap:98 L32/48000/2\r\n") != std::string::npos);
    auto const parsed = sdp::parse(text);
    EXPECT_TRUE(parsed.has_value());
    RecordingSink sink;
    Receiver receiver{STEREO_L32, sink.handler(), transport};
    EXPECT_TRUE(receiver.configure(*parsed));
    EXPECT_EQ(STEREO_L32.bytes_per_frame(), uint32_t{8});
}

// A packet is handed to the transport departing at the time of the tick that sends it, whatever
// timescale the caller ticks in (the sending NIC's PHC: PTP time, or a member interface's domain
// time); a tick without a time sends untimed.
TEST(aes67_sender, a_packet_departs_at_the_time_of_the_tick_that_sends_it)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};  // 48 frames a packet at 48 kHz
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004, .payload_type = 98}));
    EXPECT_EQ(sender.read_delay(), uint32_t{6});
    constexpr uint64_t T0 = 1'700'000'000'000'000'000ULL;
    sender.tick(54, T0 + 1'125'000);  // the tick that sends packet 0 (last frame 47, read delay 6)
    sender.tick(60, T0 + 1'250'000);  // nothing due
    sender.tick(102, T0 + 2'125'000);
    EXPECT_TRUE((transport.departures == std::vector<uint64_t>{T0 + 1'125'000, T0 + 2'125'000}));
    sender.tick(150, 0);  // untimed
    EXPECT_TRUE(!transport.departures.empty() && transport.departures.back() == 0);
    // A stalled sender catches up in one tick: every packet of the burst departs at that tick.
    sender.tick(300, T0 + 6'250'000);
    EXPECT_TRUE(transport.departures.size() >= 5 && transport.departures.back() == T0 + 6'250'000);
}

TEST(aes67_sender, parses_a_grandmaster)
{
    auto const gm = parse_grandmaster("00-1D-C1-FF-FE-12-34-56:3");
    EXPECT_TRUE(gm.has_value());
    EXPECT_EQ(gm->domain, uint8_t{3});
    EXPECT_TRUE(gm->identity == *ieee::eui64_from_string("00-1D-C1-FF-FE-12-34-56"));
    auto const colons = parse_grandmaster("00:1d:c1:ff:fe:12:34:56:0");
    EXPECT_TRUE(colons.has_value());
    EXPECT_EQ(colons->domain, uint8_t{0});
    auto const bare = parse_grandmaster("001DC1FFFE123456");
    EXPECT_TRUE(bare.has_value());
    EXPECT_FALSE(parse_grandmaster("not a clock").has_value());
    EXPECT_FALSE(parse_grandmaster("00-1D-C1-FF-FE-12-34-56:x").has_value());
    EXPECT_FALSE(parse_grandmaster("00-1D-C1-FF-FE-12-34-56:128").has_value());
}

TEST(aes67_sender, writes_sdp_that_parses_back)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);  // these tests time packets by their last frame
    EXPECT_FALSE(sender.session().has_value());
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004, .payload_type = 98}));
    sender.set_grandmaster(GRANDMASTER);

    auto const session = sender.session();
    EXPECT_TRUE(session.has_value());
    std::string const text = sdp::to_string(*session);
    auto const parsed = sdp::parse(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->origin_address, std::string{"192.168.1.10"});
    EXPECT_EQ(parsed->name, std::string{"Test"});
    EXPECT_EQ(parsed->media.size(), size_t{1});
    auto const& media = parsed->media[0];
    EXPECT_EQ(media.port, uint16_t{5004});
    EXPECT_EQ(media.payload_type, uint8_t{98});
    EXPECT_TRUE(media.encoding == sdp::Encoding::l24);
    EXPECT_EQ(media.channels, uint16_t{2});
    EXPECT_EQ(media.sample_rate, uint32_t{48000});
    EXPECT_EQ(media.packet_time_us, uint32_t{1000});
    EXPECT_EQ(media.frames_per_packet(), FPP);
    EXPECT_TRUE(parsed->effective_direction(media) == sdp::Direction::recvonly);
    EXPECT_TRUE(parsed->effective_media_clock_offset(media) == uint32_t{0});
    auto const clocks = parsed->effective_clocks(media);
    EXPECT_EQ(clocks.size(), size_t{1});
    EXPECT_TRUE(clocks.front().grandmaster == ieee::eui64_from_string("00-1D-C1-FF-FE-12-34-56"));
    auto const connection = parsed->effective_connection(media);
    EXPECT_TRUE(connection.has_value());
    EXPECT_TRUE(connection->address == (IPv4Address{239, 69, 1, 2}));
    EXPECT_EQ(connection->ttl, uint8_t{32});
    auto const filter = parsed->effective_source_filter(media);
    EXPECT_TRUE(filter.has_value());
    EXPECT_TRUE(filter->source == (IPv4Address{192, 168, 1, 10}));
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=mediaclk:direct=0\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=clock-domain:PTPv2 0\r\n") != std::string::npos);
    EXPECT_TRUE(parsed->effective_clock_domain(media) == uint8_t{0});

    // Reconfiguring bumps the version and keeps the session id.
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 3}, .port = 5004}));
    auto const again = sender.session();
    EXPECT_EQ(again->session_id, session->session_id);
    EXPECT_EQ(again->session_version, session->session_version + 1);

    // A grandmaster in another domain names it, one version on; the same
    // grandmaster again changes nothing; none leaves ts-refclk out.
    sender.set_grandmaster(Grandmaster{.identity = GRANDMASTER.identity, .domain = 5});
    EXPECT_TRUE(
        sdp::to_string(*sender.session()).find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:5\r\n") != std::string::npos);
    EXPECT_EQ(sender.session()->session_version, again->session_version + 1);
    EXPECT_EQ(sender.session()->session_id, session->session_id);
    sender.set_grandmaster(Grandmaster{.identity = GRANDMASTER.identity, .domain = 5});
    EXPECT_EQ(sender.session()->session_version, again->session_version + 1);
    sender.set_grandmaster(std::nullopt);
    EXPECT_EQ(sender.session()->session_version, again->session_version + 2);
    EXPECT_TRUE(sdp::to_string(*sender.session()).find("ts-refclk") == std::string::npos);
    EXPECT_TRUE(sdp::to_string(*sender.session()).find("clock-domain") == std::string::npos);

    // A new name is a new s=, one version on, the same session id; the same name changes nothing.
    sender.set_name("Bridge : Test");
    EXPECT_EQ(sender.name(), std::string{"Bridge : Test"});
    EXPECT_EQ(sender.session()->name, std::string{"Bridge : Test"});
    EXPECT_EQ(sender.session()->session_version, again->session_version + 3);
    EXPECT_EQ(sender.session()->session_id, session->session_id);
    sender.set_name("Bridge : Test");
    EXPECT_EQ(sender.session()->session_version, again->session_version + 3);
    // A replacement sender continues the session under its name.
    Sender replacement{STEREO_L24, source.provider(), transport, "Test"};
    replacement.continue_from(sender);
    EXPECT_EQ(replacement.name(), std::string{"Bridge : Test"});
}

TEST(aes67_sender, an_802_1as_grandmaster_is_named_without_a_domain)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    sender.set_grandmaster(Grandmaster{.identity = GRANDMASTER.identity, .domain = 0, .version = sdp::PtpVersion::ieee802_1as});
    std::string const text = sdp::to_string(*sender.session());
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE802.1AS-2011:00-1D-C1-FF-FE-12-34-56\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("clock-domain") == std::string::npos);  // RAVENNA's PTPv2 domain: none for gPTP
    auto const parsed = sdp::parse(text);
    EXPECT_TRUE(parsed && parsed->names_clock(parsed->media.at(0), GRANDMASTER.reference()));  // domain 0, one clock
}

TEST(aes67_sender, writes_a_125_us_ptime)
{
    LoopTransport transport;
    PatternSource source{6};
    auto format = STEREO_L24;
    format.frames_per_packet = 6;
    Sender sender{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    std::string const text = sdp::to_string(*sender.session());
    EXPECT_TRUE(text.find("a=ptime:0.125\r\n") != std::string::npos);
    EXPECT_EQ(sdp::parse(text)->media[0].frames_per_packet(), uint32_t{6});
}

TEST(aes67_sender, refuses_what_aes67_cannot_carry)
{
    LoopTransport transport;
    PatternSource source{6};
    auto format = STEREO_L24;
    format.sample_rate = 44100;
    Sender rate{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(rate.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}).error() == Aes67Error::unsupported_format);

    format = STEREO_L24;
    format.channels = 16;  // 48 frames of 16 x L24 is 2304 bytes
    Sender big{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(big.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}).error() == Aes67Error::packet_too_large);
}

TEST(aes67_sender, payloads_are_at_most_1440_octets)
{
    LoopTransport transport;
    PatternSource source{6};
    // AES67-2018 Section 6.3: 60 x L24 x 8 frames is 1440 octets, 2 x L16 x 361 is 1444.
    StreamFormat format{.encoding = sdp::Encoding::l24, .channels = 60, .sample_rate = 48000, .frames_per_packet = 8};
    Sender fits{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(fits.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}).has_value());
    format = {.encoding = sdp::Encoding::l16, .channels = 2, .sample_rate = 48000, .frames_per_packet = 361};
    Sender over{format, source.provider(), transport, "Test"};
    EXPECT_TRUE(over.configure(Destination{.address = IPv4Address{239, 69, 1, 3}}).error() == Aes67Error::packet_too_large);
}

TEST(aes67_sender, sends_packets_on_the_grid_when_due)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);  // these tests time packets by their last frame
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));

    uint64_t const base = 48'000ULL * 1'000'000;  // some PTP time well past the epoch
    sender.tick(base + 46, 0);                    // packet [base - 48, base) is due: last index base - 1
    EXPECT_EQ(transport.queue.size(), size_t{1});
    sender.tick(base + 46, 0);  // nothing new
    EXPECT_EQ(transport.queue.size(), size_t{1});
    sender.tick(base + 47, 0);  // [base, base + 48) completes
    EXPECT_EQ(transport.queue.size(), size_t{2});
    sender.tick(base + 47 + (3 * 48), 0);  // three more at once
    EXPECT_EQ(transport.queue.size(), size_t{5});

    uint16_t expected_seq = 0;
    for (size_t i = 0; i < transport.queue.size(); ++i) {
        auto const packet = rtp::parse_rtp(transport.queue[i].bytes);
        EXPECT_TRUE(packet.has_value());
        uint64_t const first = base - 48 + (i * 48);
        EXPECT_EQ(packet->header.timestamp.get(), static_cast<uint32_t>(first));
        EXPECT_EQ(packet->header.ssrc.get(), sender.ssrc());
        EXPECT_EQ(packet->header.payload_type(), uint8_t{96});
        EXPECT_FALSE(packet->header.marker());
        if (i > 0) {
            EXPECT_EQ(packet->header.sequence.get(), expected_seq);
        }
        expected_seq = static_cast<uint16_t>(packet->header.sequence.get() + 1);
        EXPECT_EQ(packet->payload.size(), size_t{FPP} * 6);
        EXPECT_EQ(packet->payload[0], pattern(first, 0));
        EXPECT_EQ(packet->payload[(47 * 6) + 5], pattern(first + 47, 5));
    }
    EXPECT_EQ(sender.counters().packets, uint64_t{5});
    EXPECT_EQ(sender.counters().frames, uint64_t{5 * 48});
    EXPECT_TRUE(sender.state() == State::running);

    // Reset: the counters start again, the run (its SSRC and sequence) goes on.
    uint32_t const ssrc = sender.ssrc();
    sender.reset_counters();
    EXPECT_EQ(sender.counters().packets, uint64_t{0});
    sender.tick(base + 47 + (4 * 48), 0);
    EXPECT_EQ(sender.counters().packets, uint64_t{1});
    EXPECT_EQ(transport.queue.size(), size_t{6});
    auto const next = rtp::parse_rtp(transport.queue.back().bytes);
    EXPECT_TRUE(next && next->header.ssrc.get() == ssrc && next->header.sequence.get() == expected_seq);
    EXPECT_TRUE(sender.state() == State::running);
}

TEST(aes67_sender, sends_silence_for_frames_not_held_and_skips_when_far_behind)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);  // these tests time packets by their last frame
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));

    uint64_t const base = 4800 * 48;
    sender.tick(base - 1, 0);  // sends [base - 48, base)
    source.refuse_index = base;
    sender.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    auto const silent = rtp::parse_rtp(transport.queue[1].bytes);
    EXPECT_TRUE(std::ranges::all_of(silent->payload, [](uint8_t b) { return b == 0; }));
    EXPECT_EQ(sender.counters().lost, uint64_t{48});

    // 100 packets later: only the newest is sent, the rest counted lost.
    transport.queue.clear();
    sender.tick(base + 47 + (100 * 48), 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_EQ(rtp::parse_rtp(transport.queue[0].bytes)->header.timestamp.get(), static_cast<uint32_t>(base + (100 * 48)));
    EXPECT_EQ(sender.counters().lost, uint64_t{48 + (99 * 48)});
}

TEST(aes67_sender, keeps_its_session_id_across_restarts)
{
    // Two senders for the same stream, as before and after a restart: the
    // same o= session id, and a version at least the wall clock's seconds.
    auto const start = [](std::string name, IPv4Address const& group) {
        auto transport = std::make_unique<LoopTransport>();
        auto sender = std::make_unique<Sender>(STEREO_L24, FrameProvider{}, *transport, std::move(name));
        EXPECT_TRUE(sender->configure(Destination{.address = group, .port = 5004}));
        auto session = *sender->session();
        return session;
    };
    auto const first = start("Test", IPv4Address{239, 69, 1, 2});
    auto const again = start("Test", IPv4Address{239, 69, 1, 2});
    EXPECT_EQ(again.session_id, first.session_id);
    EXPECT_NE(first.session_id, uint64_t{0});
    EXPECT_EQ(first.session_id >> 63U, uint64_t{0});  // positive in a signed reading
    EXPECT_TRUE(first.session_version > uint64_t{1'700'000'000});
    EXPECT_TRUE(again.session_version >= first.session_version);
    EXPECT_NE(start("Other", IPv4Address{239, 69, 1, 2}).session_id, first.session_id);
    EXPECT_EQ(start("Test", IPv4Address{239, 69, 1, 3}).session_id, first.session_id);  // moved, not a new stream

    // One the caller gives is kept (to 63 bits), across reconfigures too.
    LoopTransport transport;
    Sender sender{STEREO_L24, FrameProvider{}, transport, "Test"};
    sender.set_session_id(0xFFFF'0000'0000'1234ULL);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    EXPECT_EQ(sender.session()->session_id, uint64_t{0x7FFF'0000'0000'1234ULL});
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 4}}));
    EXPECT_EQ(sender.session()->session_id, uint64_t{0x7FFF'0000'0000'1234ULL});
    // A grandmaster known before the destination: the version still starts at the clock.
    Sender early{STEREO_L24, FrameProvider{}, transport, "Test"};
    early.set_grandmaster(GRANDMASTER);
    EXPECT_TRUE(early.configure(Destination{.address = IPv4Address{239, 69, 1, 5}}));
    EXPECT_TRUE(early.session()->session_version > uint64_t{1'700'000'000});
    EXPECT_EQ(Sender::stable_session_id("aes67-out-0"), Sender::stable_session_id("aes67-out-0"));
    EXPECT_NE(Sender::stable_session_id("aes67-out-0"), Sender::stable_session_id("aes67-out-1"));
}

TEST(aes67_sender, waits_out_the_read_delay)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    EXPECT_EQ(sender.read_delay(), uint32_t{6});  // the default: one 125 us tick
    EXPECT_EQ(Sender::default_read_delay(StreamFormat{.encoding = sdp::Encoding::l24, .sample_rate = 44100}), uint32_t{6});
    EXPECT_EQ(Sender::default_read_delay(StreamFormat{.encoding = sdp::Encoding::l24, .sample_rate = 96000}), uint32_t{12});
    EXPECT_EQ(Sender::relay_read_delay(STEREO_L24), FPP + 12);  // a packet and two ticks
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    sender.set_read_delay(100);

    uint64_t const base = 48'000ULL * 1000;  // a multiple of 48
    sender.tick(base + 47 + 99, 0);          // [base, base + 48) is one frame short of due
    std::size_t const before = transport.queue.size();
    sender.tick(base + 47 + 100, 0);  // due now
    EXPECT_EQ(transport.queue.size(), before + 1);
    EXPECT_EQ(rtp::parse_rtp(transport.queue.back().bytes)->header.timestamp.get(), static_cast<uint32_t>(base));
}

TEST(aes67_sender, sends_uncounted_silence_with_no_provider)
{
    LoopTransport transport;
    Sender sender{STEREO_L24, FrameProvider{}, transport, "Test"};
    sender.set_read_delay(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    sender.tick(48'000ULL * 1000 + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_TRUE(std::ranges::all_of(rtp::parse_rtp(transport.queue[0].bytes)->payload, [](uint8_t b) { return b == 0; }));
    EXPECT_EQ(sender.counters().lost, uint64_t{0});
    EXPECT_EQ(sender.counters().packets, uint64_t{1});
}

TEST(aes67_sender, a_paused_sender_sends_nothing_and_resumes_on_the_grid_as_a_new_source)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    uint64_t const base = 48'000ULL * 1'000'000;
    sender.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_TRUE(sender.state() == State::running);

    sender.pause();
    EXPECT_TRUE(sender.state() == State::idle);
    EXPECT_EQ(sender.send_state().ssrc, uint32_t{0});
    auto const last = rtp::parse_rtp(transport.queue.back().bytes);

    // Ten packets later: a new run from the newest due packet under a new SSRC (RFC 3550 Section 8).
    sender.tick(base + 47 + (10 * uint64_t{FPP}), 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    auto const resumed = rtp::parse_rtp(transport.queue.back().bytes);
    EXPECT_TRUE(last && resumed);
    EXPECT_EQ(resumed->header.timestamp.get(), static_cast<uint32_t>(base + (10 * uint64_t{FPP})));
    EXPECT_NE(resumed->header.ssrc.get(), last->header.ssrc.get());
    EXPECT_NE(resumed->header.ssrc.get(), uint32_t{0});
    EXPECT_EQ(resumed->header.ssrc.get(), sender.ssrc());
    EXPECT_TRUE(sender.state() == State::running);
    EXPECT_EQ(sender.send_state().ssrc, sender.ssrc());
    EXPECT_EQ(sender.counters().lost, uint64_t{0});

    Sender unconfigured{STEREO_L24, source.provider(), transport, "None"};
    unconfigured.pause();
    EXPECT_TRUE(unconfigured.state() == State::idle);
}

TEST(aes67_sender, a_paused_sender_whose_transport_fails_reports_error)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    uint64_t const base = 48'000ULL * 1'000'000;
    sender.tick(base + 47, 0);
    transport.failing = std::error_code{EINVAL, std::system_category()};
    sender.tick(base + 95, 0);
    EXPECT_TRUE(sender.state() == State::error);
    sender.pause();
    EXPECT_TRUE(sender.state() == State::error);  // paused, but the transport still cannot send
    EXPECT_TRUE(sender.error() == std::errc::invalid_argument);
    sender.pause();
    EXPECT_TRUE(sender.state() == State::error);
    transport.failing.clear();
    sender.pause();
    EXPECT_TRUE(sender.state() == State::idle);
    EXPECT_FALSE(sender.error());
}

namespace {

/// Whether every big-endian word of `bytes` is `word`.
auto all_words_are(std::span<uint8_t const> const bytes, uint32_t const word) -> bool
{
    if (bytes.size() % 4 != 0) {
        return false;
    }
    for (size_t at = 0; at < bytes.size(); at += 4) {
        ieee::quadlet_t got{};
        span_load(got, bytes.subspan(at, 4));
        if (got.get() != word) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(aes67_stream, silence_is_zero_for_linear_pcm_and_the_mbla_word_for_am824)
{
    EXPECT_EQ(silence_word(sdp::Encoding::l16), uint32_t{0});
    EXPECT_EQ(silence_word(sdp::Encoding::l24), uint32_t{0});
    EXPECT_EQ(silence_word(sdp::Encoding::l32), uint32_t{0});
    EXPECT_EQ(silence_word(sdp::Encoding::am824), uint32_t{0x4000'0000});

    for (size_t const size : {size_t{4}, size_t{8}, size_t{12}, size_t{60}, size_t{1440}}) {
        std::vector<uint8_t> bytes(size, 0x55);
        fill_silence(sdp::Encoding::am824, bytes);
        EXPECT_TRUE(all_words_are(bytes, 0x4000'0000));
        std::ranges::fill(bytes, uint8_t{0x55});
        fill_silence(sdp::Encoding::l24, bytes);
        EXPECT_TRUE(std::ranges::all_of(bytes, [](uint8_t b) { return b == 0; }));
    }
}

TEST(aes67_sender, am824_silence_is_the_mbla_word_in_every_sample)
{
    constexpr StreamFormat AM824{.encoding = sdp::Encoding::am824, .channels = 3, .sample_rate = 48000, .frames_per_packet = FPP};
    uint64_t const base = 48'000ULL * 1000;

    // No provider: uncounted silence, every channel's sample the MBLA word.
    LoopTransport transport;
    Sender idle{AM824, FrameProvider{}, transport, "Test"};
    idle.set_read_delay(0);
    EXPECT_TRUE(idle.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    idle.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    auto const silent = rtp::parse_rtp(transport.queue[0].bytes);
    EXPECT_EQ(silent->payload.size(), size_t{AM824.bytes_per_packet()});
    EXPECT_TRUE(all_words_are(silent->payload, 0x4000'0000));
    EXPECT_EQ(idle.counters().lost, uint64_t{0});

    // A provider that wrote part of the packet and reports it missing: all silence, counted lost.
    transport.queue.clear();
    FrameProvider partial = [](uint64_t, uint32_t, std::span<uint8_t> const out) {
        std::ranges::fill(out.first(out.size() / 2), uint8_t{0x55});
        return Supply::missing;
    };
    Sender sender{AM824, std::move(partial), transport, "Test"};
    sender.set_read_delay(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 3}}));
    sender.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_TRUE(all_words_are(rtp::parse_rtp(transport.queue[0].bytes)->payload, 0x4000'0000));
    EXPECT_EQ(sender.counters().lost, uint64_t{FPP});

    // The same provider on L24: zero bytes, as before.
    transport.queue.clear();
    FrameProvider partial_l24 = [](uint64_t, uint32_t, std::span<uint8_t> const out) {
        std::ranges::fill(out.first(out.size() / 2), uint8_t{0x55});
        return Supply::missing;
    };
    Sender l24{STEREO_L24, std::move(partial_l24), transport, "Test"};
    l24.set_read_delay(0);
    EXPECT_TRUE(l24.configure(Destination{.address = IPv4Address{239, 69, 1, 4}}));
    l24.tick(base + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_TRUE(std::ranges::all_of(rtp::parse_rtp(transport.queue[0].bytes)->payload, [](uint8_t b) { return b == 0; }));
}

TEST(aes67_sender, waits_out_a_small_backward_step)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    uint64_t const base = 48'000ULL * 1000;
    for (uint64_t k = 0; k < 4; ++k) {
        sender.tick(base + 47 + (k * FPP), 0);  // sends [base, ...) .. [base + 3 * 48, ...)
    }
    EXPECT_EQ(transport.queue.size(), size_t{4});
    // Two packets back: nothing is sent again, and sending resumes where it left off.
    sender.tick(base + 47 + FPP, 0);
    sender.tick(base + 47 + (3 * FPP), 0);
    EXPECT_EQ(transport.queue.size(), size_t{4});
    sender.tick(base + 47 + (4 * FPP), 0);
    EXPECT_EQ(transport.queue.size(), size_t{5});
    EXPECT_EQ(rtp::parse_rtp(transport.queue.back().bytes)->header.timestamp.get(), static_cast<uint32_t>(base + (4 * FPP)));
    for (size_t i = 1; i < transport.queue.size(); ++i) {
        auto const previous = rtp::parse_rtp(transport.queue[i - 1].bytes)->header.timestamp.get();
        EXPECT_EQ(rtp::parse_rtp(transport.queue[i].bytes)->header.timestamp.get(), previous + FPP);  // none repeated
    }
}

TEST(aes67_sender, resynchronises_after_a_large_backward_step)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}}));
    uint64_t const base = 48'000ULL * 1000;
    sender.tick(base + 47, 0);
    // A step back of more than MAX_BURST packets: start again from now.
    uint64_t const back = base - (uint64_t{Sender::MAX_BURST + 2} * FPP);
    sender.tick(back + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    EXPECT_EQ(rtp::parse_rtp(transport.queue.back().bytes)->header.timestamp.get(), static_cast<uint32_t>(back));
}

TEST(aes67_sender, still_pulls_and_counts_a_packet_the_transport_cannot_take)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));

    uint64_t const base = 4800 * 48;
    sender.tick(base - 1, 0);  // [base - 48, base), rendered in place
    EXPECT_EQ(source.pulls, size_t{1});
    // No TX buffer: the frames are still taken (into the staging buffer), the
    // missing packet still counted lost, and both packets counted dropped.
    transport.refuse = true;
    source.refuse_index = base + 48;
    sender.tick(base + 95, 0);
    EXPECT_EQ(source.pulls, size_t{3});
    EXPECT_EQ(transport.queue.size(), size_t{1});
    EXPECT_EQ(sender.counters().dropped, uint64_t{2});
    EXPECT_EQ(sender.counters().lost, uint64_t{48});
    EXPECT_EQ(sender.counters().packets, uint64_t{1});
}

TEST(aes67_sender, a_packet_phase_moves_the_send_tick_not_the_timestamps)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);
    sender.set_packet_phase(18);
    EXPECT_EQ(sender.packet_phase(), uint32_t{18});
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));

    uint64_t const base = 48'000ULL * 1'000'000;  // a multiple of 48
    sender.tick(base + 16, 0);                    // starts with [base - 78, base - 30)
    EXPECT_EQ(transport.queue.size(), size_t{1});
    sender.tick(base + 16, 0);  // [base - 30, base + 18) completes at base + 17
    EXPECT_EQ(transport.queue.size(), size_t{1});
    sender.tick(base + 17, 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    sender.tick(base + 17 + 47, 0);
    EXPECT_EQ(transport.queue.size(), size_t{2});
    sender.tick(base + 17 + 48, 0);  // [base + 18, base + 66)
    EXPECT_EQ(transport.queue.size(), size_t{3});
    for (size_t i = 0; i < transport.queue.size(); ++i) {
        auto const packet = rtp::parse_rtp(transport.queue[i].bytes);
        uint64_t const first = base - 78 + (i * 48);
        // The timestamp is still the first frame's index: receivers place the samples exactly.
        EXPECT_EQ(packet->header.timestamp.get(), static_cast<uint32_t>(first));
        EXPECT_EQ(packet->payload[0], pattern(first, 0));
        EXPECT_EQ(packet->payload[(47 * 6) + 5], pattern(first + 47, 5));
    }
    // The phase is taken modulo the packet.
    sender.set_packet_phase(48 + 6);
    EXPECT_EQ(sender.packet_phase(), uint32_t{6});
}

TEST(aes67_sender, stagger_phases_spread_eight_senders_over_eight_ticks)
{
    // 1 ms at 48 kHz: 0, 6, ... 42, then again.
    for (uint32_t k = 0; k < 16; ++k) {
        EXPECT_EQ(Sender::stagger_phase(STEREO_L24, k), (k % 8) * 6);
    }
    // 96 kHz: a tick is 12 frames.
    StreamFormat const hi{.encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 96000, .frames_per_packet = 96};
    EXPECT_EQ(Sender::stagger_phase(hi, 3), uint32_t{36});
    // A packet of one tick: every sender sends every tick anyway.
    StreamFormat const short_packets{.encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = 6};
    EXPECT_EQ(Sender::stagger_phase(short_packets, 5), uint32_t{0});
    // 250 us: two ticks.
    StreamFormat const quarter{.encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = 12};
    EXPECT_EQ(Sender::stagger_phase(quarter, 1), uint32_t{6});
    EXPECT_EQ(Sender::stagger_phase(quarter, 2), uint32_t{0});
}

TEST(aes67_sender, reports_the_transports_send_error)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Test"};
    sender.set_read_delay(0);
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    uint64_t const base = 4800 * 48;
    sender.tick(base - 1, 0);
    EXPECT_TRUE(sender.state() == State::running);
    transport.failing = std::error_code{EINVAL, std::system_category()};
    sender.tick(base + 47, 0);
    EXPECT_TRUE(sender.state() == State::error);
    EXPECT_TRUE(sender.error() == std::errc::invalid_argument);
    sender.tick(base + 47, 0);  // nothing due: still in error
    EXPECT_TRUE(sender.state() == State::error);
    transport.failing.clear();
    sender.tick(base + 47, 0);  // nothing due, but the transport sends again
    EXPECT_TRUE(sender.state() == State::ready);
    EXPECT_FALSE(sender.error());
    sender.tick(base + 95, 0);
    EXPECT_TRUE(sender.state() == State::running);
}

TEST(aes67_sender, a_sender_on_a_drifting_index_sends_every_packet_once)
{
    // A domain 100 ppm fast or slow of PTP: ticks on the 125 us PTP grid see its index advance by
    // 5, 6 or 7 frames, and a 125 us stream sends 0, 1 or 2 packets a tick.
    auto const clock = *mediaclock::PtpMediaClock::make(48000);
    uint64_t const t0 = 1'790'000'000ULL * 1'000'000'000ULL;
    for (uint32_t const fpp : {6U, 48U}) {
        for (double const rate_offset : {100e-6, -100e-6}) {
            LoopTransport transport;
            PatternSource source{6};
            auto format = STEREO_L24;
            format.frames_per_packet = fpp;
            Sender sender{format, source.provider(), transport, "Drift"};
            EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
            mediaclock::Timebase const timebase{
                .reference_ns = static_cast<int64_t>(t0),
                .domain_ns = static_cast<int64_t>(t0),
                .rate_offset = rate_offset,
                .running = true};
            std::array<size_t, 9> advances{};
            uint64_t previous = 0;
            for (uint64_t tick = 0; tick < 24'000; ++tick) {  // 3 s
                auto const t = static_cast<int64_t>(t0 + (tick * 125'000));
                uint64_t const index = clock.index_at(static_cast<uint64_t>(timebase.to_domain(t)));
                if (tick > 0) {
                    ++advances[std::min<uint64_t>(index - previous, advances.size() - 1)];
                }
                previous = index;
                sender.tick(index, 0);
            }
            EXPECT_EQ(advances[6] + advances[rate_offset > 0 ? 7 : 5], size_t{23'999});
            EXPECT_TRUE(advances[rate_offset > 0 ? 7 : 5] > 0);

            // Every packet once, in order: timestamps one packet apart, sequence numbers one apart.
            EXPECT_TRUE(transport.queue.size() > 1);
            auto const first = rtp::parse_rtp(transport.queue.front().bytes);
            uint32_t const first_ts = first->header.timestamp.get();
            uint16_t const first_seq = first->header.sequence.get();
            bool in_order = true;
            for (size_t i = 0; i < transport.queue.size(); ++i) {
                auto const packet = rtp::parse_rtp(transport.queue[i].bytes);
                uint32_t const ts = packet->header.timestamp.get();
                in_order = in_order && ts == first_ts + static_cast<uint32_t>(i * fpp) &&
                    packet->header.sequence.get() == static_cast<uint16_t>(first_seq + i) && packet->payload[0] == pattern(ts, 0);
            }
            EXPECT_TRUE(in_order);
            EXPECT_EQ(sender.counters().packets, uint64_t{transport.queue.size()});
            EXPECT_EQ(sender.counters().lost, uint64_t{0});
            EXPECT_EQ(sender.counters().dropped, uint64_t{0});
        }
    }
}

TEST(aes67_sender, a_foreign_domain_output_announces_mediaclk_sender)
{
    LoopTransport transport;
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), transport, "Foreign"};
    EXPECT_TRUE(sender.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    sender.set_grandmaster(GRANDMASTER);
    auto const before = *sender.session();
    uint8_t const info_version = sender.info_block_version();
    EXPECT_FALSE(sender.sender_clock());

    sender.set_sender_clock(true);
    EXPECT_TRUE(sender.sender_clock());
    EXPECT_TRUE(sender.media_clock().kind() == sdp::MediaClockKind::sender);
    auto const session = *sender.session();
    EXPECT_EQ(session.session_version, before.session_version + 1);
    EXPECT_EQ(sender.info_block_version(), static_cast<uint8_t>(info_version + 1));
    std::string const text = sdp::to_string(session);
    EXPECT_TRUE(text.find("a=mediaclk:sender\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("direct") == std::string::npos);
    // The reference the sender reports are in stays the grandmaster (RFC 7273 Section 4.8).
    EXPECT_TRUE(text.find("a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=clock-domain:PTPv2 0\r\n") != std::string::npos);
    sender.set_sender_clock(true);
    EXPECT_EQ(sender.session()->session_version, session.session_version);

    // A direct receiver refuses it; one taking the sender's clock takes it and waits for an offset.
    auto const parsed = sdp::parse(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->effective_media_clock(parsed->media[0])->kind() == sdp::MediaClockKind::sender);
    Receiver direct{STEREO_L24, {}, transport};
    EXPECT_TRUE(direct.configure(*parsed).error() == Aes67Error::not_ptp_locked);
    Receiver foreign{STEREO_L24, {}, transport};
    foreign.set_sender_clock(true);
    EXPECT_TRUE(foreign.configure(*parsed).has_value());
    EXPECT_FALSE(foreign.offset().has_value());

    // A replacement sender keeps announcing it.
    Sender replacement{STEREO_L24, source.provider(), transport, "Foreign"};
    replacement.continue_from(sender);
    EXPECT_TRUE(replacement.configure(Destination{.address = IPv4Address{239, 69, 1, 2}, .port = 5004}));
    EXPECT_TRUE(sdp::to_string(*replacement.session()).find("a=mediaclk:sender\r\n") != std::string::npos);
}

TEST(aes67_receiver, a_receiver_taking_the_senders_clock_refuses_a_direct_stream)
{
    LoopTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    receiver.set_sender_clock(true);
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)).error() == Aes67Error::not_sender_clocked);
    EXPECT_TRUE(receiver.configure(stream_session(group, std::nullopt)).error() == Aes67Error::not_sender_clocked);
    auto other = stream_session(group, std::nullopt);
    other.media[0].media_clock = sdp::MediaClock{.other = true};  // e.g. IEEE1722=<stream id>
    EXPECT_TRUE(receiver.configure(other).error() == Aes67Error::not_sender_clocked);
    EXPECT_FALSE(receiver.configured());

    // A direct receiver's offset is the SDP's.
    Receiver direct{STEREO_L24, {}, transport};
    EXPECT_TRUE(direct.configure(stream_session(group, 0x1000)).has_value());
    EXPECT_TRUE(direct.offset() == std::optional<uint32_t>{0x1000});
}

TEST(aes67_receiver, refuses_streams_it_cannot_take)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};

    EXPECT_TRUE(receiver.configure(stream_session(group, std::nullopt)).error() == Aes67Error::not_ptp_locked);
    auto session = stream_session(group);
    session.media[0].channels = 8;
    EXPECT_TRUE(receiver.configure(session).error() == Aes67Error::format_mismatch);
    session = stream_session(group);
    session.media[0].sample_rate = 44100;
    EXPECT_TRUE(receiver.configure(session).error() == Aes67Error::unsupported_format);
    session = stream_session(group);
    session.media[0].encoding = sdp::Encoding::am824;
    EXPECT_TRUE(receiver.configure(session).error() == Aes67Error::encoding_mismatch);
    EXPECT_TRUE(receiver.configure(stream_session(group), 1).error() == Aes67Error::no_media);
    EXPECT_FALSE(receiver.configured());
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    EXPECT_TRUE(receiver.configured());
    EXPECT_TRUE(receiver.state() == State::ready);
}

TEST(aes67_receiver, takes_the_offered_format_in_its_own_encoding)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L32, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};

    // only L24 offered: refused for its sample width; an L24 receiver refuses L32 alike
    auto session = stream_session(group);
    EXPECT_TRUE(receiver.configure(session).error() == Aes67Error::encoding_mismatch);
    session.media[0].encoding = sdp::Encoding::l32;
    Receiver l24{STEREO_L24, sink.handler(), transport};
    EXPECT_TRUE(l24.configure(session).error() == Aes67Error::encoding_mismatch);

    // L24 first, L32 as payload type 96: the L32 receiver takes 96, not 97
    session = stream_session(group);
    EXPECT_TRUE(session.media[0].add_alternative(
        {.payload_type = 96, .encoding = sdp::Encoding::l32, .sample_rate = 48000, .channels = 2}));
    EXPECT_TRUE(receiver.configure(session));
    uint64_t const index = (1ULL << 32U) + 96;
    auto send = [&](uint8_t const pt, size_t const frame_bytes) {
        std::vector<uint8_t> const payload(size_t{FPP} * frame_bytes, uint8_t{0x5A});
        rtp::RtpHeader header{};
        header.init(pt, 10, static_cast<uint32_t>(index), 0x1234);
        std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + payload.size());
        (void)rtp::serialize_rtp(header, payload, bytes);
        transport.queue.push_back({group, std::move(bytes)});
    };
    send(97, 6);
    send(96, 8);
    receiver.drain(index + FPP);
    EXPECT_EQ(sink.blocks.size(), size_t{1});
    EXPECT_TRUE(sink.blocks.size() == 1 && sink.blocks[0].frames == FPP && sink.blocks[0].bytes.size() == size_t{FPP} * 8);
}

TEST(aes67_receiver, pushes_frames_by_index_and_counts_loss_reorder_and_strays)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    uint32_t const offset = 0x1000;
    EXPECT_TRUE(receiver.configure(stream_session(group, offset)));

    // Indices just past 2^32 so the timestamps wrap: the reference unwraps them.
    uint64_t const base = (1ULL << 32U) + 96;
    auto send = [&](uint16_t const seq, uint64_t const index, uint32_t const ssrc = 0x1234, uint8_t const pt = 97) {
        auto bytes = rtp_packet(seq, static_cast<uint32_t>(index), ssrc, pt);
        // Timestamps carry the offset; the pattern is by index.
        rtp::RtpHeader header{};
        header.init(pt, seq, static_cast<uint32_t>(index) + offset, ssrc);
        (void)store_unchecked(std::span{bytes}, header);
        transport.queue.push_back({group, std::move(bytes)});
    };
    auto at = [&](uint16_t const n) { return base + (uint64_t{n} * FPP); };

    send(10, at(0));  // on probation, handed on all the same
    send(11, at(1));
    send(12, at(2));
    send(14, at(4));                                   // 13 missing
    send(13, at(3));                                   // ...arrives late
    send(13, at(3));                                   // duplicate
    send(16, at(6));                                   // 15 lost for good
    send(17, at(7), 0x9999);                           // another source
    send(17, at(7), 0x1234, 96);                       // another payload type
    transport.queue.push_back({group, {0x80, 0x61}});  // malformed
    receiver.drain(at(7));

    EXPECT_EQ(sink.blocks.size(), size_t{6});
    std::array<uint16_t, 6> const order{0, 1, 2, 4, 3, 6};
    for (size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ(sink.blocks[i].first_index, at(order[i]));
        EXPECT_EQ(sink.blocks[i].frames, FPP);
        expect_pattern(sink.blocks[i]);
    }
    auto const& c = receiver.counters();
    EXPECT_EQ(c.packets, uint64_t{6});
    EXPECT_EQ(c.frames, uint64_t{6 * FPP});
    EXPECT_EQ(c.lost, uint64_t{FPP});   // 15; 13 was recovered
    EXPECT_EQ(c.late, uint64_t{FPP});   // 13
    EXPECT_EQ(c.dropped, uint64_t{4});  // duplicate, source, payload type, malformed
    EXPECT_EQ(receiver.ssrc(), std::optional<uint32_t>{0x1234});
    EXPECT_TRUE(receiver.state() == State::running);

    // Reset: the counters start again; the source and its sequence are kept, so the next packet
    // is placed as before, the gap behind it (17) is a loss and a repeat is still a duplicate.
    receiver.reset_counters();
    EXPECT_TRUE(receiver.counters() == Counters{});
    send(18, at(8));
    send(18, at(8));
    receiver.drain(at(8));
    EXPECT_EQ(receiver.ssrc(), std::optional<uint32_t>{0x1234});
    EXPECT_EQ(receiver.counters().packets, uint64_t{1});
    EXPECT_EQ(receiver.counters().lost, uint64_t{FPP});
    EXPECT_EQ(receiver.counters().dropped, uint64_t{1});
    EXPECT_EQ(sink.blocks.back().first_index, at(8));

    // A second of silence from 0x1234, and a new source is adopted.
    uint64_t const later = at(7) + 48000;
    send(500, later, 0x5555);
    send(501, later + FPP, 0x5555);
    receiver.drain(later + FPP);
    EXPECT_EQ(receiver.ssrc(), std::optional<uint32_t>{0x5555});
    EXPECT_EQ(sink.blocks.back().first_index, later + FPP);
}

TEST(aes67_receiver, each_refused_packet_is_counted_under_its_reason)
{
    LoopTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    auto session = stream_session(group);
    session.source_filter = sdp::SourceFilter{.destination = std::nullopt, .source = transport.source.address};
    EXPECT_TRUE(receiver.configure(session));
    uint64_t const base = 48'000ULL * 4000;
    auto send = [&](uint16_t const n, uint32_t const ssrc = 0x1234, uint8_t const pt = 97) {
        transport.queue.push_back({group, rtp_packet(n, static_cast<uint32_t>(base + (uint64_t{n} * FPP)), ssrc, pt)});
    };
    send(0);
    send(1);
    send(1);                                                                        // sequence: a duplicate
    send(2, 0x9999);                                                                // ssrc: another source while 0x1234 is live
    send(2, 0x1234, 96);                                                            // payload type
    transport.queue.push_back({group, {0x80, 0x61}});                               // not RTP
    transport.queue.push_back({group, {0x80, 200, 0x00, 0x06, 0, 0, 0x12, 0x34}});  // RTCP SR (RFC 5761): not refused
    auto empty = rtp_packet(2, static_cast<uint32_t>(base + (uint64_t{2} * FPP)));
    empty.resize(rtp::RtpHeader::LENGTH);
    transport.queue.push_back({group, empty});  // size: no frames
    auto ragged = rtp_packet(2, static_cast<uint32_t>(base + (uint64_t{2} * FPP)));
    ragged.pop_back();
    transport.queue.push_back({group, ragged});  // size: not whole frames
    receiver.drain(base + (uint64_t{2} * FPP));
    transport.source.address = IPv4Address{192, 168, 1, 99};
    send(2);  // source: not the filter's
    receiver.drain(base + (uint64_t{3} * FPP));

    auto const& c = receiver.counters();
    EXPECT_EQ(c.packets, uint64_t{2});
    EXPECT_EQ(c.drops.sequence, uint64_t{1});
    EXPECT_EQ(c.drops.ssrc, uint64_t{1});
    EXPECT_EQ(c.drops.payload_type, uint64_t{1});
    EXPECT_EQ(c.drops.not_rtp, uint64_t{1});
    EXPECT_EQ(c.drops.size, uint64_t{2});
    EXPECT_EQ(c.drops.source, uint64_t{1});
    EXPECT_EQ(c.drops.no_offset, uint64_t{0});
    EXPECT_EQ(c.dropped, uint64_t{7});
}

TEST(aes67_receiver, a_sender_clocked_stream_counts_packets_before_its_offset_as_no_offset)
{
    LoopTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    receiver.set_sender_clock(true);
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    auto session = stream_session(group, std::nullopt);
    session.media[0].media_clock = sdp::MediaClock::sender();
    EXPECT_TRUE(receiver.configure(session));
    uint64_t const base = 48'000ULL * 5000;
    transport.queue.push_back({group, rtp_packet(0, static_cast<uint32_t>(base))});
    transport.queue.push_back({group, rtp_packet(1, static_cast<uint32_t>(base + FPP))});
    receiver.drain(base + FPP);
    EXPECT_EQ(receiver.counters().drops.no_offset, uint64_t{2});
    EXPECT_EQ(receiver.counters().dropped, uint64_t{2});
    receiver.set_offset(0);
    transport.queue.push_back({group, rtp_packet(2, static_cast<uint32_t>(base + (uint64_t{2} * FPP)))});
    receiver.drain(base + (uint64_t{2} * FPP));
    EXPECT_EQ(receiver.counters().packets, uint64_t{1});
    EXPECT_EQ(receiver.counters().dropped, uint64_t{2});
}

TEST(aes67_receiver, takes_an_eight_channel_stream_in_packets_of_any_whole_frame_count)
{
    // One sender's announcement: source port = destination port, a large media clock offset.
    constexpr char const* SDP_TEXT =
        "v=0\r\no=dmixv2 37878419 1793158514 IN IP4 10.10.32.163\r\ns=dmixv2_01\r\nc=IN IP4 239.200.163.140/32\r\n"
        "t=0 0\r\nm=audio 5004 RTP/AVP 99\r\ni=8 channels\r\na=recvonly\r\na=rtpmap:99 L24/48000/8\r\na=ptime:1\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-10-6C-16:0\r\na=mediaclk:direct=4045604396\r\n";
    constexpr StreamFormat EIGHT_L24{.encoding = sdp::Encoding::l24, .channels = 8, .sample_rate = 48000, .frames_per_packet = FPP};
    LoopTransport transport;
    transport.source = {.address = IPv4Address{10, 10, 32, 163}, .port = 5004};
    RecordingSink sink;
    Receiver receiver{EIGHT_L24, sink.handler(), transport};
    auto const session = sdp::parse(SDP_TEXT);
    EXPECT_TRUE(session && receiver.configure(*session));
    Endpoint const group{.address = IPv4Address{239, 200, 163, 140}, .port = 5004};

    // 48 frames (a 1206-octet frame), then 24, then Sender::MAX_PAYLOAD's 60.
    uint64_t const base = (1ULL << 40U) + 12345;
    std::array<uint32_t, 3> const sizes{FPP, FPP / 2, Sender::MAX_PAYLOAD / 24};
    uint64_t index = base;
    for (uint16_t i = 0; i < sizes.size(); ++i) {
        rtp::RtpHeader header{};
        header.init(99, static_cast<uint16_t>(0xCA34 + i), static_cast<uint32_t>(index) + 4045604396U, 0x0241FA93);
        std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + (size_t{sizes[i]} * 24));
        (void)rtp::serialize_rtp_header(header, bytes.size() - rtp::RtpHeader::LENGTH, bytes);
        transport.queue.push_back({group, std::move(bytes)});
        index += sizes[i];
    }
    receiver.drain(index + 53);

    EXPECT_EQ(sink.blocks.size(), sizes.size());
    EXPECT_EQ(receiver.counters().dropped, uint64_t{0});
    index = base;
    for (size_t i = 0; i < std::min(sizes.size(), sink.blocks.size()); ++i) {
        EXPECT_EQ(sink.blocks[i].first_index, index);
        EXPECT_EQ(sink.blocks[i].frames, sizes[i]);
        index += sizes[i];
    }
}

TEST(aes67_receiver, takes_late_packets_as_far_back_as_rtp_sequence_does)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 2000;
    auto send = [&](uint16_t const n) {
        transport.queue.push_back({group, rtp_packet(n, static_cast<uint32_t>(base + (uint64_t{n} * FPP)))});
    };
    // 0..200 except 136 and 101, then those two: 64 and 99 behind the high-water mark 200.
    for (uint16_t n = 0; n <= 200; ++n) {
        if (n != 136 && n != 101 && n != 100) {
            send(n);
        }
    }
    while (!transport.queue.empty()) {
        receiver.drain(base + (201 * FPP));  // a drain takes at most MAX_BATCHES batches
    }
    auto const lost = receiver.counters().lost;
    EXPECT_EQ(lost, uint64_t{3 * FPP});
    send(136);                           // 64 behind
    send(101);                           // 99 behind: RtpSequence's last late packet
    send(100);                           // 100 behind: RtpSequence takes it for a jump
    receiver.drain(base + (202 * FPP));  // a packet time on: past the due-tick gate
    EXPECT_EQ(receiver.counters().late, uint64_t{2 * FPP});
    EXPECT_EQ(receiver.counters().lost, uint64_t{FPP});
    EXPECT_EQ(receiver.counters().dropped, uint64_t{1});
    EXPECT_EQ(sink.blocks.back().first_index, base + (101 * FPP));
}

TEST(aes67_receiver, polls_a_queuing_port_only_when_a_packet_can_be_due)
{
    LoopTransport transport;
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), transport};
    Endpoint const group{.address = IPv4Address{239, 69, 1, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(group)));
    uint64_t const base = 48'000ULL * 3000;  // a multiple of 48
    auto send = [&](uint16_t const n) {
        transport.queue.push_back({group, rtp_packet(n, static_cast<uint32_t>(base + (uint64_t{n} * FPP)))});
    };
    // Before the first packet: every tick.
    for (uint64_t t = 0; t < 4; ++t) {
        receiver.drain(base + (t * 6));
    }
    EXPECT_EQ(transport.receives, size_t{4});
    // Packet n, [base + 48n, base + 48(n + 1)), arrives a tick after its last frame.
    size_t polls_at_start = 0;
    for (uint16_t n = 0; n < 10; ++n) {
        if (n == 2) {
            polls_at_start = transport.receives;
        }
        uint64_t const arrival = base + (uint64_t{n + 1U} * FPP) + 6;
        for (uint64_t now = arrival - 48; now < arrival; now += 6) {
            receiver.drain(now);
        }
        send(n);
        receiver.drain(arrival);
    }
    EXPECT_EQ(receiver.counters().packets, uint64_t{10});
    // In steady state: from a tick before the next packet can be sent to the
    // tick it arrives, three polls per eight ticks.
    EXPECT_EQ(transport.receives - polls_at_start, size_t{8 * 3});
    EXPECT_TRUE(receiver.skipped_polls() > 0);

    // A lost packet: polled every tick past due until the next one.
    size_t const before = transport.receives;
    uint64_t const due = base + (11 * FPP) - 6;
    for (uint64_t now = due; now < due + 96; now += 6) {
        receiver.drain(now);
    }
    EXPECT_EQ(transport.receives - before, size_t{16});

    // A transport whose ports keep a tick's datagrams only: every tick.
    transport.every_tick = true;
    send(12);
    receiver.drain(base + (13 * FPP));
    size_t const xdp_before = transport.receives;
    for (uint64_t t = 1; t <= 8; ++t) {
        receiver.drain(base + (13 * FPP) + (t * 6));
    }
    EXPECT_EQ(transport.receives - xdp_before, size_t{8});
}

TEST(aes67_udp, two_receivers_share_a_port)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    uint16_t const port = 45030;
    Endpoint const unicast{.address = own_loopback(), .port = port};
    Endpoint const group{.address = own_group(), .port = port};
    RecordingSink unicast_sink;
    Receiver unicast_receiver{STEREO_L24, unicast_sink.handler(), **transport};
    EXPECT_TRUE(unicast_receiver.configure(stream_session(unicast)));
    RecordingSink group_sink;
    Receiver group_receiver{STEREO_L24, group_sink.handler(), **transport};
    bool const multicast = group_receiver.configure(stream_session(group)).has_value();

    // A second unicast receiver on the same address and port is refused.
    RecordingSink other_sink;
    Receiver other{STEREO_L24, other_sink.handler(), **transport};
    EXPECT_TRUE(other.configure(stream_session(unicast)).error() == std::errc::address_in_use);

    auto socket = net::make_udp_socket();
    EXPECT_TRUE(socket.has_value());
    EXPECT_TRUE(net::set_multicast_send(socket->get(), ::if_nametoindex("lo"), 1, true).has_value());
    auto send_to = [&](Endpoint const& to, uint16_t const seq) {
        auto const bytes = rtp_packet(seq, static_cast<uint32_t>(seq * FPP));
        sockaddr_in const address = to_sockaddr(to);
        return ::sendto(socket->get(), bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr const*>(&address), sizeof(address));
    };
    for (uint16_t seq = 0; seq < 3; ++seq) {
        EXPECT_TRUE(send_to(unicast, seq) > 0);
        if (multicast) {
            (void)send_to(group, static_cast<uint16_t>(seq + 1000));
        }
    }
    uint64_t const now = 1000 * FPP;
    for (int tries = 0; tries < 100 && unicast_receiver.counters().packets < 3; ++tries) {
        unicast_receiver.drain(now);
        group_receiver.drain(now);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    unicast_receiver.drain(now);
    group_receiver.drain(now);
    EXPECT_EQ(unicast_receiver.counters().packets, uint64_t{3});
    EXPECT_EQ(unicast_receiver.counters().dropped, uint64_t{0});  // the group's datagrams never reach it
    if (!multicast || group_receiver.counters().packets == 0) {
        std::println("  skipped (multicast part): no multicast on lo here");
        return;
    }
    EXPECT_EQ(group_receiver.counters().packets, uint64_t{3});
    EXPECT_EQ(group_receiver.counters().dropped, uint64_t{0});  // nor the unicast ones it
}

TEST(aes67_udp, sender_to_receiver_over_loopback)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    EXPECT_TRUE((*transport)->local_address() == (IPv4Address{127, 0, 0, 1}));

    Endpoint const destination{.address = own_loopback(), .port = 45004};
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), **transport, "Test"};
    sender.set_read_delay(0);  // these tests time packets by their last frame
    EXPECT_TRUE(sender.configure(Destination{.address = destination.address, .port = destination.port, .payload_type = 97}));
    sender.set_grandmaster(GRANDMASTER);

    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), **transport};
    // The receiver configured from the sender's own SDP, as text and back.
    auto const sdp_text = sdp::to_string(*sender.session());
    auto const parsed = sdp::parse(sdp_text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(receiver.configure(*parsed));

    // A fake PTP clock: 1 ms (one packet) per step, from an index past 2^32.
    uint64_t now = (5ULL << 32U) + 47;
    for (int step = 0; step < 10; ++step) {
        sender.tick(now, 0);
        receiver.drain(now);
        for (int tries = 0; tries < 100 && receiver.counters().packets < static_cast<uint64_t>(step + 1); ++tries) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            receiver.drain(now);
        }
        now += FPP;
    }
    receiver.drain(now);
    EXPECT_EQ(receiver.counters().packets, uint64_t{10});
    EXPECT_EQ(sink.blocks.size(), size_t{10});
    // Packets lie on the 48-frame grid; the second one sent is the first kept.
    uint64_t const first = ((((5ULL << 32U) + 48) / FPP) * FPP) - FPP;  // the first packet sent
    for (size_t i = 0; i < sink.blocks.size(); ++i) {
        EXPECT_EQ(sink.blocks[i].first_index, first + (i * FPP));
        expect_pattern(sink.blocks[i]);
    }
    EXPECT_EQ(receiver.counters().lost, uint64_t{0});
    EXPECT_EQ(receiver.ssrc(), std::optional<uint32_t>{sender.ssrc()});
    EXPECT_EQ(sender.counters().packets, uint64_t{10});
}

TEST(aes67_udp, unicast_to_a_closed_port_is_not_refused)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), **transport, "Test"};
    sender.set_read_delay(0);
    // Nothing listens on this port: each datagram draws an ICMP port-unreachable.
    EXPECT_TRUE(sender.configure(Destination{.address = own_loopback(), .port = 45010}));
    uint64_t now = 48'000ULL * 900 + 47;
    for (int step = 0; step < 20; ++step, now += FPP) {
        sender.tick(now, 0);
        std::this_thread::sleep_for(std::chrono::microseconds{200});  // let the ICMP come back
    }
    EXPECT_EQ(sender.counters().dropped, uint64_t{0});
    EXPECT_EQ(sender.counters().packets, uint64_t{20});

    // A listener that appears later gets what follows.
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), **transport};
    auto session = stream_session({.address = own_loopback(), .port = 45010});
    session.media[0].payload_type = 96;
    EXPECT_TRUE(receiver.configure(session));
    for (int step = 0; step < 3; ++step, now += FPP) {
        sender.tick(now, 0);
    }
    for (int tries = 0; tries < 100 && receiver.counters().packets < 3; ++tries) {
        receiver.drain(now);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(receiver.counters().packets, uint64_t{3});
    EXPECT_EQ(sender.counters().dropped, uint64_t{0});
}

TEST(aes67_udp, counts_loss_and_reorder_from_the_wire)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const destination{.address = own_loopback(), .port = 45006};
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), **transport};
    EXPECT_TRUE(receiver.configure(stream_session(destination)));

    auto socket = net::make_udp_socket();
    EXPECT_TRUE(socket.has_value());
    sockaddr_in const to = to_sockaddr(destination);
    uint64_t const base = 48000ULL * 3600;
    for (uint16_t const n : {0, 1, 2, 4, 3, 6, 7}) {
        auto const bytes = rtp_packet(static_cast<uint16_t>(100 + n), static_cast<uint32_t>(base + (n * FPP)));
        EXPECT_EQ(
            ::sendto(socket->get(), bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr const*>(&to), sizeof(to)),
            static_cast<ssize_t>(bytes.size()));
    }
    uint64_t const now = base + (8 * FPP);
    for (int tries = 0; tries < 100 && receiver.counters().packets < 7; ++tries) {
        receiver.drain(now);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(receiver.counters().packets, uint64_t{7});
    EXPECT_EQ(receiver.counters().late, uint64_t{FPP});  // 3 after 4
    EXPECT_EQ(receiver.counters().lost, uint64_t{FPP});  // 5
    EXPECT_EQ(sink.blocks.back().first_index, base + (7 * FPP));
    expect_pattern(sink.blocks.back());
}

TEST(aes67_udp, a_truncated_datagram_is_counted_malformed)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const unicast{.address = own_loopback(), .port = 45012};
    auto port = (*transport)->open_receive(unicast);
    EXPECT_TRUE(port.has_value());
    auto socket = net::make_udp_socket();
    EXPECT_TRUE(socket.has_value());
    std::vector<uint8_t> const oversized(UdpTransport::MAX_DATAGRAM + 100, uint8_t{0x80});
    sockaddr_in const address = to_sockaddr(unicast);
    EXPECT_TRUE(
        ::sendto(
            socket->get(), oversized.data(), oversized.size(), 0, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) >
        0);
    std::array<Datagram, 4> out{};
    for (int tries = 0; tries < 200 && (*transport)->receive_refusals().malformed == 0; ++tries) {
        EXPECT_EQ((*port)->receive(out), size_t{0});
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ((*transport)->receive_refusals().malformed, uint64_t{1});
    EXPECT_EQ((*transport)->receive_refusals().unmatched, uint64_t{0});
    (*transport)->reset_counters();
    EXPECT_EQ((*transport)->receive_refusals(), ReceiveRefusals{});
    port->reset();
}

TEST(aes67_udp, multicast_over_loopback_when_the_host_allows)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const group{.address = own_group(), .port = 45008};
    RecordingSink sink;
    Receiver receiver{STEREO_L24, sink.handler(), **transport};
    if (!receiver.configure(stream_session(group))) {
        std::println("  skipped: cannot join a group on lo");
        return;
    }
    PatternSource source{6};
    Sender sender{STEREO_L24, source.provider(), **transport, "Test"};
    sender.set_read_delay(0);  // these tests time packets by their last frame
    EXPECT_TRUE(sender.configure(Destination{.address = group.address, .port = group.port, .payload_type = 97}));
    uint64_t now = 48000ULL * 7200 + 47;
    for (int step = 0; step < 5; ++step, now += FPP) {
        sender.tick(now, 0);
    }
    for (int tries = 0; tries < 50 && receiver.counters().packets < 5; ++tries) {
        receiver.drain(now);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (sender.counters().dropped != 0 || receiver.counters().packets == 0) {
        std::println("  skipped: the host does not route multicast on lo");
        return;
    }
    EXPECT_EQ(receiver.counters().packets, uint64_t{5});
    expect_pattern(sink.blocks.back());
}

namespace {

/// Wait for `count` datagrams on `port`, appending copies to `out`.
void receive_datagrams(ReceivePort& port, size_t const count, std::vector<std::vector<uint8_t>>& out)
{
    std::array<Datagram, UdpTransport::RECEIVE_BATCH> batch{};
    for (int tries = 0; tries < 200 && out.size() < count; ++tries) {
        size_t const n = port.receive(batch);
        for (size_t i = 0; i < n; ++i) {
            out.emplace_back(batch[i].payload.begin(), batch[i].payload.end());
        }
        if (out.size() < count) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
}

/// The bridge's AES67 encodings (L16, L24 and L32), at a few channel counts.
constexpr std::array<StreamFormat, 6> RENDER_FORMATS{{
    {.encoding = sdp::Encoding::l16, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = sdp::Encoding::l16, .channels = 8, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = sdp::Encoding::l24, .channels = 8, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = sdp::Encoding::l32, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP},
    {.encoding = sdp::Encoding::l32, .channels = 6, .sample_rate = 48000, .frames_per_packet = FPP},  // 8 would not fit
}};

}  // namespace

namespace {

/// Wait for a datagram on `port`; its payload copied, or empty after 200 ms.
auto receive_rtcp(RtcpPort& port) -> std::pair<std::vector<uint8_t>, Endpoint>
{
    std::array<Datagram, 4> batch{};
    for (int tries = 0; tries < 200; ++tries) {
        if (port.receive(batch) > 0) {
            return {{batch[0].payload.begin(), batch[0].payload.end()}, batch[0].source};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return {};
}

}  // namespace

TEST(aes67_udp, an_rtcp_port_sends_to_and_receives_from_its_group)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    RtcpOptions const options{.destination = {.address = own_group(), .port = 45101}, .ttl = 1, .dscp = 34};
    auto a = (*transport)->open_rtcp(options);
    auto b = (*transport)->open_rtcp(options);
    if (!a || !b) {
        std::println("  skipped: cannot join a group on lo");
        return;
    }
    EXPECT_TRUE((*a)->fd() >= 0);
    int tos = 0;
    socklen_t length = sizeof(tos);
    EXPECT_EQ(::getsockopt((*a)->fd(), IPPROTO_IP, IP_TOS, &tos, &length), 0);
    EXPECT_EQ(tos, 34 << 2);  // AES67-2018 Section 6.2: the stream's DSCP

    std::array<uint8_t, 8> const packet{0x80, 0xC9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07};
    EXPECT_TRUE((*a)->send(packet).has_value());
    auto const [bytes, source] = receive_rtcp(**b);
    if (bytes.empty()) {
        std::println("  skipped: the host does not route multicast on lo");
        return;
    }
    EXPECT_EQ(bytes, (std::vector<uint8_t>{packet.begin(), packet.end()}));
    EXPECT_EQ(source.port, uint16_t{45101});
    EXPECT_EQ((*b)->dropped(), uint64_t{0});
}

TEST(aes67_udp, a_unicast_rtcp_port_reaches_its_peer)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    IPv4Address const x = own_loopback();
    IPv4Address const y{127, static_cast<uint8_t>(x.octet(1) + 64), x.octet(2), x.octet(3)};
    uint16_t const port = 45111;
    auto a = (*transport)->open_rtcp({.destination = {.address = y, .port = port}, .local = {.address = x, .port = port}});
    auto b = (*transport)->open_rtcp({.destination = {.address = x, .port = port}, .local = {.address = y, .port = port}});
    EXPECT_TRUE(a.has_value());
    EXPECT_TRUE(b.has_value());
    // A unicast address and port is one port's: a second is refused rather than splitting its reports.
    auto again = (*transport)->open_rtcp({.destination = {.address = y, .port = port}, .local = {.address = x, .port = port}});
    EXPECT_TRUE(again.error() == std::errc::address_in_use);
    std::array<uint8_t, 8> const packet{0x80, 0xC9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x09};
    EXPECT_TRUE((*a)->send(packet).has_value());
    auto const [to_b, from_a] = receive_rtcp(**b);
    EXPECT_EQ(to_b.size(), packet.size());
    EXPECT_TRUE(from_a == (Endpoint{.address = x, .port = port}));
    EXPECT_TRUE((*b)->send(packet).has_value());
    auto const [to_a, from_b] = receive_rtcp(**a);
    EXPECT_EQ(to_a.size(), packet.size());
    EXPECT_TRUE(from_b == (Endpoint{.address = y, .port = port}));
}

TEST(aes67_udp, multicast_receive_sockets_take_only_their_own_joins)
{
    auto const lo = find_interface("lo");
    EXPECT_TRUE(lo.has_value());
    auto const multicast_all = [](int const fd) {
        int all = -1;
        socklen_t length = sizeof(all);
        EXPECT_EQ(::getsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &all, &length), 0);
        return all;
    };
    auto const unicast = open_receive_socket(*lo, {.address = own_loopback(), .port = 45040}, true);
    EXPECT_TRUE(unicast.has_value());
    EXPECT_EQ(multicast_all(unicast->get()), 1);  // the kernel's default: nothing joined to filter by
    auto const group = open_receive_socket(*lo, {.address = own_group(), .port = 45040}, true);
    auto transport = UdpTransport::open("lo");
    auto rtcp = (*transport)->open_rtcp({.destination = {.address = own_group(), .port = 45041}});
    if (!group || !rtcp) {
        std::println("  skipped: cannot join a group on lo");
        return;
    }
    EXPECT_EQ(multicast_all(group->get()), 0);
    EXPECT_EQ(multicast_all((*rtcp)->fd()), 0);
}

TEST(aes67_udp, a_datagram_is_kept_only_for_its_address_and_interface)
{
    alignas(cmsghdr) std::array<uint8_t, PKTINFO_SPACE> control{};
    msghdr header{};
    header.msg_control = control.data();
    header.msg_controllen = control.size();
    EXPECT_FALSE(destined_for(header, IPv4Address{239, 69, 1, 2}));  // no IP_PKTINFO
    cmsghdr* const cmsg = CMSG_FIRSTHDR(&header);
    cmsg->cmsg_level = IPPROTO_IP;
    cmsg->cmsg_type = IP_PKTINFO;
    cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
    in_pktinfo info{};
    info.ipi_ifindex = 3;
    uint32_t const group = IPv4Address{239, 69, 1, 2}.to_uint32();  // htonl is a macro: no commas in its argument
    info.ipi_addr.s_addr = htonl(group);
    std::memcpy(CMSG_DATA(cmsg), &info, sizeof(info));
    EXPECT_TRUE(destined_for(header, IPv4Address{239, 69, 1, 2}));      // any interface
    EXPECT_TRUE(destined_for(header, IPv4Address{239, 69, 1, 2}, 3));   // its own
    EXPECT_FALSE(destined_for(header, IPv4Address{239, 69, 1, 2}, 4));  // another NIC's join
    EXPECT_FALSE(destined_for(header, IPv4Address{239, 69, 1, 3}, 3));
}

TEST(aes67_udp, rendered_sends_match_copied_sends_byte_for_byte)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const destination{.address = own_loopback(), .port = 45020};
    auto rx = (*transport)->open_receive(destination);
    EXPECT_TRUE(rx.has_value());
    auto tx = (*transport)->open_send({.destination = destination});
    EXPECT_TRUE(tx.has_value());
    auto& port = **tx;

    uint16_t seq = 100;
    for (auto const& format : RENDER_FORMATS) {
        std::vector<uint8_t> payload(format.bytes_per_packet());
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = pattern(seq, i);
        }
        rtp::RtpHeader header{};
        header.init(97, seq, 0x12345678U + seq, 0xCAFEF00DU);
        std::vector<uint8_t> expected(rtp::RtpHeader::LENGTH + payload.size());
        EXPECT_TRUE(rtp::serialize_rtp(header, payload, expected).has_value());

        EXPECT_TRUE(port.send(header, payload));
        size_t rendered_length = 0;
        EXPECT_TRUE(port.send_rendered(header, payload.size(), [&](std::span<uint8_t> const out) noexcept {
            rendered_length = out.size();
            std::ranges::copy(payload, out.begin());
            return true;
        }));
        EXPECT_EQ(rendered_length, payload.size());
        // An abandoned render takes no batch slot and sends nothing.
        EXPECT_FALSE(port.send_rendered(header, payload.size(), [](std::span<uint8_t> const out) noexcept {
            std::ranges::fill(out, uint8_t{0xEE});
            return false;
        }));
        EXPECT_EQ(port.flush(), size_t{0});

        std::vector<std::vector<uint8_t>> got;
        receive_datagrams(**rx, 3, got);
        EXPECT_EQ(got.size(), size_t{2});
        EXPECT_TRUE(got.size() == 2 && got[0] == expected && got[1] == expected);
        ++seq;
    }

    // A full batch: send_rendered() refuses without calling render, and the
    // flush still sends the whole batch, per tick as before.
    std::array<uint8_t, 12> const small{};
    rtp::RtpHeader header{};
    header.init(97, 1, 2, 3);
    for (size_t i = 0; i < UdpTransport::SEND_BATCH; ++i) {
        EXPECT_TRUE(port.send_rendered(header, small.size(), [](std::span<uint8_t> const out) noexcept {
            std::ranges::fill(out, uint8_t{1});
            return true;
        }));
    }
    bool called = false;
    EXPECT_FALSE(port.send_rendered(header, small.size(), [&](std::span<uint8_t>) noexcept {
        called = true;
        return true;
    }));
    EXPECT_FALSE(called);
    EXPECT_FALSE(port.send(header, small));
    EXPECT_EQ(port.flush(), size_t{0});
    std::vector<std::vector<uint8_t>> got;
    receive_datagrams(**rx, UdpTransport::SEND_BATCH, got);
    EXPECT_EQ(got.size(), UdpTransport::SEND_BATCH);
    // Too large for a datagram: refused, render not called.
    EXPECT_FALSE(port.send_rendered(header, UdpTransport::MAX_DATAGRAM, [&](std::span<uint8_t>) noexcept {
        called = true;
        return true;
    }));
    EXPECT_FALSE(called);
    EXPECT_EQ(port.flush(), size_t{0});
}

TEST(aes67_udp, sender_renders_each_payload_once_straight_into_the_batch)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());

    uint16_t port_number = 45030;
    for (auto const& format : RENDER_FORMATS) {
        Endpoint const destination{.address = own_loopback(), .port = port_number++};
        auto rx = (*transport)->open_receive(destination);
        EXPECT_TRUE(rx.has_value());

        // Counts each pull and where it wrote: with a staging copy every pull
        // would land in the same buffer; rendered in place, each packet of a
        // tick lands in its own batch slot, right after its RTP header.
        uint32_t const frame_bytes = format.bytes_per_frame();
        PatternSource source{frame_bytes};
        std::vector<uint8_t const*> targets;
        size_t bytes_written = 0;
        FrameProvider provider = [&](uint64_t const first_index, uint32_t const frames, std::span<uint8_t> const out) {
            targets.push_back(out.data());
            EXPECT_EQ(out.size(), size_t{frames} * frame_bytes);
            if (!source.pull(first_index, frames, out)) {
                std::ranges::fill(out, uint8_t{0x55});  // a partial pull: the sender must still send silence
                return Supply::missing;
            }
            bytes_written += out.size();
            return Supply::frames;
        };
        Sender sender{format, std::move(provider), **transport, "Test"};
        sender.set_read_delay(0);
        EXPECT_TRUE(sender.configure(Destination{.address = destination.address, .port = destination.port, .payload_type = 98}));

        uint64_t const base = 48'000ULL * 2000;
        source.refuse_index = base + 48;  // the second packet of the burst is missing
        sender.tick(base - 1, 0);         // one packet: [base - 48, base)
        sender.tick(base + 143, 0);       // a burst of three in one tick
        EXPECT_EQ(source.pulls, size_t{4});
        EXPECT_EQ(targets.size(), size_t{4});
        for (size_t i = 2; i < targets.size(); ++i) {
            EXPECT_EQ(targets[i] - targets[i - 1], static_cast<std::ptrdiff_t>(UdpTransport::MAX_DATAGRAM));
        }
        EXPECT_EQ(bytes_written, size_t{3} * format.bytes_per_packet());  // one copy per packet, nothing more
        EXPECT_EQ(sender.counters().packets, uint64_t{4});
        EXPECT_EQ(sender.counters().lost, uint64_t{FPP});
        EXPECT_EQ(sender.counters().dropped, uint64_t{0});

        // Each datagram as the copy-in path built it: the provider's frames
        // (silence for the missing packet) behind the same header.
        std::vector<std::vector<uint8_t>> got;
        receive_datagrams(**rx, 4, got);
        EXPECT_EQ(got.size(), size_t{4});
        for (size_t i = 0; i < got.size(); ++i) {
            auto const packet = rtp::parse_rtp(got[i]);
            EXPECT_TRUE(packet.has_value());
            uint64_t const first = base - 48 + (i * 48);
            EXPECT_EQ(packet->header.timestamp.get(), static_cast<uint32_t>(first));
            std::vector<uint8_t> payload(format.bytes_per_packet(), 0);
            if (first != source.refuse_index) {
                (void)source.pull(first, FPP, payload);
            }
            std::vector<uint8_t> expected(rtp::RtpHeader::LENGTH + payload.size());
            EXPECT_TRUE(rtp::serialize_rtp(packet->header, payload, expected).has_value());
            EXPECT_TRUE(got[i] == expected);
        }
    }
}

TEST_MAIN(statusbar_aes67, aes67_test)
