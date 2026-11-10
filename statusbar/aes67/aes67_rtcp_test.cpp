// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_rtcp.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/aes67/aes67_udp_transport.hpp"
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/mediaclock/mediaclock_timebase.hpp"
#include "statusbar/rtp/rtcp_ipmx.hpp"
#include "statusbar/rtp/rtcp_packet.hpp"
#include "statusbar/rtp/rtcp_time.hpp"
#include "statusbar/rtp/rtp_header.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::aes67;
using statusbar::ip::IPv4Address;

namespace {

constexpr StreamFormat STEREO_L24{.encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = 48};
constexpr uint64_t MS = 1'000'000;
/// A PTP time in 2026, on a sample boundary.
constexpr uint64_t T0 = 1'790'000'000ULL * 1'000'000'000ULL;
Endpoint const GROUP{.address = IPv4Address{239, 69, 7, 8}, .port = 5004};
Grandmaster const GRANDMASTER{.identity = *ieee::eui64_from_string("EC-46-70-FF-FE-10-FF-B0"), .domain = 0};

auto index_at(uint64_t const ptp_ns) -> uint64_t
{
    return mediaclock::PtpMediaClock::make(48000)->index_at(ptp_ns);
}

/// The time of sample `index` at 48 kHz, PTP ns.
auto time_of(uint64_t const index) -> uint64_t
{
    return mediaclock::PtpMediaClock::make(48000).transform([&](auto const& clock) { return clock.time_of(index); }).value_or(0);
}

/// The SR of the compound packet `bytes`; an empty one, failing the expectation, without.
auto sr_of(std::vector<uint8_t> const& bytes) -> rtp::RtcpSenderReport
{
    auto const compound = rtp::parse_rtcp(bytes);
    EXPECT_TRUE(compound.has_value() && compound->sr.has_value());
    return compound && compound->sr ? *compound->sr : rtp::RtcpSenderReport{};
}

/// The IPMX Info Block of the SR in `bytes`; an empty one, failing the expectation, without.
auto info_block_of(std::vector<uint8_t> const& bytes) -> rtp::IpmxInfoBlock
{
    auto const block = rtp::parse_ipmx_info_block(sr_of(bytes).extension);
    EXPECT_TRUE(block.has_value());
    return block.value_or(rtp::IpmxInfoBlock{});
}

/// The time an SR's field names, read near `reference`; 0, failing the expectation, when it does not read.
auto sr_time(rtp::RtcpSenderReport const& sr, uint64_t const reference) -> uint64_t
{
    auto const at = rtp::ns_from_ptp_truncated({.seconds = sr.info.ntp_msw.get(), .nanoseconds = sr.info.ntp_lsw.get()}, reference);
    EXPECT_TRUE(at.has_value());
    return at.value_or(0);
}

/// The same interval every time.
class FixedCadence final : public RtcpCadence
{
  public:
    explicit FixedCadence(uint64_t const ns)
        : ns_{ns}
    {}
    [[nodiscard]] auto next_interval_ns(bool) noexcept -> uint64_t override { return ns_; }

  private:
    uint64_t ns_;
};

/// A sender's options are ipmx unless `reports` says; `interval_ns` is an rfc3550 sender's or a receiver's cadence.
auto options(std::string cname, uint64_t const interval_ns = 10 * MS, SenderReports const reports = SenderReports::ipmx)
    -> RtcpSessionOptions
{
    return {.cname = std::move(cname), .reports = reports, .cadence = std::make_unique<FixedCadence>(interval_ns)};
}

/// A transport in memory: RTP sent is counted and dropped, RTP and RTCP to receive are injected,
/// RTCP sent is recorded.
class FakeTransport final : public Transport
{
  public:
    class Rx final : public ReceivePort
    {
      public:
        explicit Rx(FakeTransport& t)
            : transport{t}
        {}
        auto receive(std::span<Datagram> const out) noexcept -> size_t override
        {
            held.clear();
            size_t n = 0;
            while (n < out.size() && !transport.rtp_in.empty()) {
                held.push_back(std::move(transport.rtp_in.front()));
                transport.rtp_in.pop_front();
                out[n++] = {.payload = held.back(), .source = {.address = IPv4Address{10, 0, 0, 1}, .port = 5004}};
            }
            return n;
        }
        FakeTransport& transport;
        std::deque<std::vector<uint8_t>> held;
    };

    class Tx final : public SendPort
    {
      public:
        explicit Tx(FakeTransport& t)
            : transport{t}
        {}
        auto send_rendered(rtp::RtpHeader const& header, size_t const length, PayloadRenderer const& render) noexcept
            -> bool override
        {
            transport.log.push_back({.rtcp = false, .timestamp = header.timestamp.get()});
            std::vector<uint8_t> payload(length);
            return render(payload);
        }
        auto flush() noexcept -> size_t override { return 0; }
        FakeTransport& transport;
    };

    class Rtcp final : public RtcpPort
    {
      public:
        explicit Rtcp(FakeTransport& t)
            : transport{t}
        {}
        [[nodiscard]] auto fd() const noexcept -> int override { return -1; }
        auto receive(std::span<Datagram> const out) noexcept -> size_t override
        {
            held.clear();
            size_t n = 0;
            while (n < out.size() && !transport.rtcp_in.empty()) {
                held.push_back(std::move(transport.rtcp_in.front()));
                transport.rtcp_in.pop_front();
                out[n++] = {.payload = held.back(), .source = {.address = transport.rtcp_source, .port = 5005}};
            }
            return n;
        }
        auto send(std::span<uint8_t const> const packet) noexcept -> Status override
        {
            if (transport.refuse_rtcp) {
                return failure(std::errc::network_unreachable);
            }
            transport.rtcp_out.emplace_back(packet.begin(), packet.end());
            auto const compound = rtp::parse_rtcp(packet);
            if (compound && compound->sr) {
                transport.log.push_back({.rtcp = true, .timestamp = compound->sr->info.rtp_timestamp.get()});
            }
            return success();
        }
        FakeTransport& transport;
        std::deque<std::vector<uint8_t>> held;
    };

    [[nodiscard]] auto local_address() const noexcept -> IPv4Address override { return IPv4Address{10, 0, 0, 2}; }
    [[nodiscard]] auto local_mac() const noexcept -> std::optional<ieee::Eui48> override
    {
        return ieee::Eui48{0x00, 0x20, 0xFC, 0x32, 0x2F, 0x40};
    }
    auto open_receive(Endpoint const&) -> StatusValue<std::unique_ptr<ReceivePort>> override { return std::make_unique<Rx>(*this); }
    auto open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>> override
    {
        sends.push_back(options);
        return std::make_unique<Tx>(*this);
    }
    auto open_rtcp(RtcpOptions const& options) -> StatusValue<std::unique_ptr<RtcpPort>> override
    {
        opened.push_back(options);
        return std::make_unique<Rtcp>(*this);
    }
    [[nodiscard]] auto drain_every_tick() const noexcept -> bool override { return true; }

    std::deque<std::vector<uint8_t>> rtp_in;
    std::deque<std::vector<uint8_t>> rtcp_in;
    std::vector<std::vector<uint8_t>> rtcp_out;
    std::vector<RtcpOptions> opened;
    std::vector<SendOptions> sends;
    bool refuse_rtcp{false};               ///< the RTCP port refuses every send
    IPv4Address rtcp_source{10, 0, 0, 1};  ///< where received RTCP comes from: the stream's sender
    /// An RTP packet or SR sent, by its RTP timestamp, in the order sent.
    struct Sent
    {
        bool rtcp{false};
        uint32_t timestamp{0};
    };
    std::vector<Sent> log;
};

auto stream_session(Endpoint const& destination) -> sdp::Session
{
    sdp::Session session{};
    session.origin_address = "10.0.0.1";
    session.connection = sdp::Connection{.address = destination.address, .ttl = 32};
    sdp::Media media{};
    media.port = destination.port;
    media.payload_type = 96;
    media.encoding = sdp::Encoding::l24;
    media.channels = 2;
    media.media_clock = sdp::MediaClock{.direct_offset = 0};
    session.media.push_back(media);
    return session;
}

/// A 1 ms L24 stereo RTP packet of sample index `index`.
auto rtp_packet(uint16_t const seq, uint64_t const index, uint32_t const ssrc = 0x5EED) -> std::vector<uint8_t>
{
    rtp::RtpHeader header{};
    header.init(96, seq, static_cast<uint32_t>(index), ssrc);
    std::vector<uint8_t> const payload(STEREO_L24.bytes_per_packet(), 0);
    std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + payload.size());
    (void)rtp::serialize_rtp(header, payload, bytes);
    return bytes;
}

auto make_sr(uint32_t const ssrc, uint64_t const ptp_ns) -> std::vector<uint8_t>
{
    auto const truncated = rtp::ptp_truncated_from_ns(ptp_ns);
    rtp::SenderInfo info{};
    info.ntp_msw = truncated.seconds;
    info.ntp_lsw = truncated.nanoseconds;
    std::array<uint8_t, 128> out{};
    rtp::RtcpWriter writer{out};
    writer.sender_report(ssrc, info).sdes(ssrc, "talker@10.0.0.1");
    return {writer.bytes().begin(), writer.bytes().end()};
}

/// An SR in the IPMX form (its Info Block says the time field is PTP time) pairing `rtp_timestamp`
/// with `ptp_ns`.
auto make_ipmx_sr(uint32_t const ssrc, uint64_t const ptp_ns, uint32_t const rtp_timestamp) -> std::vector<uint8_t>
{
    auto const truncated = rtp::ptp_truncated_from_ns(ptp_ns);
    rtp::SenderInfo info{};
    info.ntp_msw = truncated.seconds;
    info.ntp_lsw = truncated.nanoseconds;
    info.rtp_timestamp = rtp_timestamp;
    rtp::IpmxInfoBlock const block{.version = 1, .ts_refclk = "ptp=IEEE1588-2008:traceable", .mediaclk = "sender"};
    std::vector<uint8_t> extension(rtp::ipmx_info_block_size(block));
    (void)rtp::serialize_ipmx_info_block(block, extension);
    std::array<uint8_t, 256> out{};
    rtp::RtcpWriter writer{out};
    writer.sender_report(ssrc, info, {}, extension).sdes(ssrc, "talker@10.0.0.1");
    return {writer.bytes().begin(), writer.bytes().end()};
}

/// A sender ticked from T0 for `ms` milliseconds, then its session ticked to its first SR.
struct SenderRig
{
    FakeTransport transport;
    Sender sender{STEREO_L24, {}, transport, "Out"};
    std::unique_ptr<RtcpSession> session;

    SenderRig()
    {
        sender.set_send_offset(0);
        sender.set_grandmaster(GRANDMASTER);
        (void)sender.configure(Destination{.address = GROUP.address, .port = GROUP.port});
        session = std::move(*RtcpSession::open_sender(sender, transport, options("bridge@10.0.0.2")));
    }

    /// Tick the sender for `ms` ms of packets and the session along, from `start`.
    void run(uint64_t const start, uint64_t const ms)
    {
        for (uint64_t t = start; t <= start + (ms * MS); t += MS) {
            sender.tick(index_at(t), 0);
            session->tick(t);
        }
    }
};

}  // namespace

TEST(aes67_rtcp, a_sender_session_opens_at_the_streams_rtcp_endpoint_and_dscp)
{
    SenderRig rig;
    EXPECT_EQ(rig.transport.opened.size(), size_t{1});
    auto const& opened = rig.transport.opened[0];
    EXPECT_TRUE(opened.destination == (Endpoint{.address = GROUP.address, .port = 5005}));
    EXPECT_EQ(opened.dscp, rtp::FrameAddressing::DSCP_MEDIA);
    EXPECT_TRUE(rig.session->role() == RtcpSession::Role::sender);
    EXPECT_EQ(rig.session->ssrc(), rig.sender.ssrc());
}

TEST(aes67_rtcp, a_senders_dscp_marks_its_stream_and_its_rtcp)
{
    FakeTransport transport;
    Sender sender{STEREO_L24, {}, transport, "Out"};
    sender.set_dscp(46);  // a Dante network's media class, not the default
    EXPECT_TRUE(sender.configure(Destination{.address = GROUP.address, .port = GROUP.port}).has_value());
    auto const session = RtcpSession::open_sender(sender, transport, options("bridge@10.0.0.2"));
    EXPECT_TRUE(session.has_value());
    EXPECT_EQ(transport.sends.size(), size_t{1});
    EXPECT_EQ(transport.sends[0].dscp, uint8_t{46});
    EXPECT_EQ(transport.opened.size(), size_t{1});
    EXPECT_EQ(transport.opened[0].dscp, uint8_t{46});
}

TEST(aes67_rtcp, media_and_rtcp_default_to_af41)
{
    FakeTransport transport;
    Sender sender{STEREO_L24, {}, transport, "Out"};
    EXPECT_EQ(sender.dscp(), uint8_t{34});  // AES67-2023 Clause 6.2 Table 1
    EXPECT_EQ(RtcpSessionOptions{}.dscp, uint8_t{34});
    EXPECT_EQ(SendOptions{}.dscp, uint8_t{34});
    EXPECT_EQ(RtcpOptions{}.dscp, uint8_t{34});
    EXPECT_EQ(rtp::FrameAddressing{}.dscp, uint8_t{34});
}

TEST(aes67_rtcp, a_sender_sends_an_ipmx_sr_and_its_cname)
{
    SenderRig rig;
    rig.run(T0, 0);  // the run's first packet is held: its SR goes at once
    EXPECT_EQ(rig.transport.rtcp_out.size(), size_t{1});
    EXPECT_EQ(rig.session->stats().sr_sent, uint64_t{1});
    auto const compound = rtp::parse_rtcp(rig.transport.rtcp_out[0]);
    EXPECT_TRUE(compound.has_value());
    EXPECT_EQ(compound->packets, size_t{2});
    auto const& sr = *compound->sr;
    EXPECT_EQ(sr.ssrc, rig.sender.ssrc());
    EXPECT_EQ(sr.blocks.count(), size_t{0});
    EXPECT_EQ(sr.info.packet_count.get(), uint32_t{0});
    EXPECT_EQ(sr.info.octet_count.get(), uint32_t{0});

    // VSF TR-10-1 Section 8.10.1: the first packet's RTP timestamp and the time its media clock is sampled.
    uint64_t const first = rig.sender.send_state().first_index;
    uint64_t const at = sr_time(sr, T0);
    EXPECT_EQ(at, time_of(first));
    EXPECT_EQ(sr.info.rtp_timestamp.get(), static_cast<uint32_t>(first));
    EXPECT_TRUE(at > T0);
    EXPECT_TRUE(at <= T0 + (uint64_t{Sender::FIRST_REPORT_LEAD_US} * 1000) + MS);

    auto const block = rtp::parse_ipmx_info_block(sr.extension);
    EXPECT_TRUE(block.has_value());
    EXPECT_EQ(block->version, rig.sender.info_block_version());
    EXPECT_EQ(block->ts_refclk, std::string_view{"ptp=IEEE1588-2008:EC-46-70-FF-FE-10-FF-B0:0"});
    EXPECT_EQ(block->mediaclk, std::string_view{"direct=0"});
    EXPECT_TRUE(block->pcm.has_value());
    EXPECT_EQ(block->pcm->sampling_rate, uint32_t{48000});
    EXPECT_EQ(block->pcm->sample_size, uint8_t{24});
    EXPECT_EQ(block->pcm->channel_count, uint8_t{2});
    EXPECT_EQ(block->pcm->packet_time_us, uint16_t{1000});
    EXPECT_EQ(block->pcm->measured_sample_rate, uint32_t{48000});
    EXPECT_EQ(block->pcm->channel_order, std::string_view{"SMPTE2110.(U02)"});
    EXPECT_EQ(compound->sdes->cname, std::string_view{"bridge@10.0.0.2"});
}

// The first packet goes a lead after the run starts: time for its SR, which the session sends at its first tick
TEST(aes67_rtcp, a_runs_first_packet_follows_its_sender_report)
{
    SenderRig rig;
    rig.sender.tick(index_at(T0), 0);
    rig.session->tick(T0);
    EXPECT_EQ(rig.transport.log.size(), size_t{1});
    uint32_t const reported = rig.transport.log.at(0).timestamp;
    for (uint64_t t = T0 + MS; t <= T0 + (30 * MS); t += MS) {
        rig.sender.tick(index_at(t), 0);
    }
    EXPECT_TRUE(rig.transport.log.size() > 1);
    EXPECT_FALSE(rig.transport.log.at(1).rtcp);
    EXPECT_EQ(rig.transport.log.at(1).timestamp, reported);
}

// Audio never waits on RTCP: with no SR at all (the session never ticks), the run starts one lead after it began
TEST(aes67_rtcp, a_run_starts_a_lead_after_it_began_whether_or_not_its_report_went)
{
    SenderRig rig;
    for (uint64_t t = T0; t <= T0 + (30 * MS); t += MS) {
        rig.sender.tick(index_at(t), 0);
    }
    auto const first = static_cast<uint32_t>(rig.sender.send_state().first_index);
    EXPECT_FALSE(rig.transport.log.empty());
    EXPECT_FALSE(rig.transport.log.empty() || rig.transport.log.front().rtcp);
    EXPECT_EQ(rig.transport.log.empty() ? 0U : rig.transport.log.front().timestamp, first);
    EXPECT_TRUE(
        index_at(T0) + 960 - 96 <= rig.sender.send_state().first_index);  // 20 ms at 48 kHz past the newest packet due at T0
}

// A step past the held packet, more than a burst, places it afresh: its SR never names a packet that is not sent
TEST(aes67_rtcp, a_clock_step_past_the_held_packet_places_it_afresh)
{
    SenderRig rig;
    rig.sender.tick(index_at(T0), 0);
    uint64_t const held = rig.sender.send_state().first_index;
    rig.sender.tick(index_at(T0 + (200 * MS)), 0);  // PTP stepped 200 ms forward
    uint64_t const placed = rig.sender.send_state().first_index;
    EXPECT_TRUE(placed > held);
    for (uint64_t t = T0 + (201 * MS); t <= T0 + (230 * MS); t += MS) {
        rig.sender.tick(index_at(t), 0);
    }
    EXPECT_FALSE(rig.transport.log.empty());
    EXPECT_EQ(rig.transport.log.empty() ? 0U : rig.transport.log.front().timestamp, static_cast<uint32_t>(placed));
    EXPECT_EQ(rig.sender.counters().lost, uint64_t{0});
}

// A run paused while its first packet is held sent no RTP: no BYE for its SSRC
TEST(aes67_rtcp, a_run_paused_before_its_first_packet_says_no_bye)
{
    SenderRig rig;
    rig.sender.tick(index_at(T0), 0);
    rig.session->tick(T0);  // its first SR
    rig.sender.pause();
    rig.session->tick(T0 + MS);
    auto const byes = std::ranges::count_if(rig.transport.rtcp_out, [](auto const& bytes) {
        auto const compound = rtp::parse_rtcp(bytes);
        return compound && compound->bye;
    });
    EXPECT_EQ(byes, 0);
    EXPECT_EQ(rig.session->stats().bye_sent, uint64_t{0});
}

// An SR that cannot go (its domain stopped) does not make the session ask to wake at once, again and again
TEST(aes67_rtcp, a_session_whose_report_cannot_go_asks_to_wake_later_not_now)
{
    itc::SeqlockValue<mediaclock::Timebase> timebase{mediaclock::Timebase{.running = true}};
    SenderRig rig;
    rig.session->set_timebase(&timebase);
    rig.run(T0, 40);
    timebase.store(mediaclock::Timebase{});  // not running: report_time() has nothing to pair
    for (uint64_t t = T0 + (41 * MS); t <= T0 + (60 * MS); t += MS) {
        rig.sender.tick(index_at(t), 0);
        rig.session->tick(t);
    }
    auto const wake = rig.session->next_wake_ns(T0 + (60 * MS));
    EXPECT_TRUE(wake.value_or(0) > T0 + (60 * MS));
}

// A session wakes midway through its next SR's window, which opens when the last SR's packet leaves
TEST(aes67_rtcp, a_sender_session_asks_to_wake_midway_through_its_next_window)
{
    SenderRig rig;
    EXPECT_FALSE(rig.session->next_wake_ns(T0).has_value());  // nothing sending
    rig.run(T0, 40);
    auto const wake = rig.session->next_wake_ns(T0 + (40 * MS));
    EXPECT_TRUE(wake.has_value());
    EXPECT_TRUE(wake.value_or(0) >= T0 + (40 * MS));
    EXPECT_TRUE(wake.value_or(0) <= T0 + (55 * MS));  // at most one and a half windows ahead
}

// rfc3550: SRs at the cadence's interval, the first packet sent at once, no IPMX declaration; the Info Block stays
// (it tells a reader how to read the time field, whatever the schedule)
TEST(aes67_rtcp, an_rfc3550_sender_reports_at_its_interval_without_holding_or_declaring_ipmx)
{
    FakeTransport transport;
    Sender sender{STEREO_L24, {}, transport, "Out"};
    sender.set_send_offset(0);
    (void)sender.configure(Destination{.address = GROUP.address, .port = GROUP.port});
    auto session =
        std::move(*RtcpSession::open_sender(sender, transport, options("bridge@10.0.0.2", 100 * MS, SenderReports::rfc3550)));
    EXPECT_FALSE(sender.holds_for_reports());
    EXPECT_FALSE(sender.session().value_or(sdp::Session{}).media.at(0).ipmx);
    for (uint64_t t = T0; t <= T0 + (500 * MS); t += MS) {
        sender.tick(index_at(t), 0);
        session->tick(t);
    }
    EXPECT_FALSE(transport.log.empty() || transport.log.front().rtcp);  // RTP from the first tick, no SR first
    auto const reports = std::ranges::count_if(transport.log, [](auto const& e) { return e.rtcp; });
    EXPECT_TRUE(reports >= 4);  // armed at T0, then one each 100 ms
    EXPECT_TRUE(reports <= 5);
    auto const block = info_block_of(transport.rtcp_out.back());
    EXPECT_EQ(block.mediaclk, std::string_view{"direct=0"});
}

// The media never waits on the RTCP path: with every SR refused by the port, the run starts all the same
TEST(aes67_rtcp, a_refused_first_sender_report_does_not_stop_the_run)
{
    SenderRig rig;
    rig.transport.refuse_rtcp = true;
    rig.run(T0, 40);
    EXPECT_TRUE(rig.session->stats().send_errors > 0);
    EXPECT_TRUE(std::ranges::any_of(rig.transport.log, [](auto const& e) { return !e.rtcp; }));
}

// A ReportHold lasts as long as it lives, moves with its owner, and the holds are counted
TEST(aes67_sender, a_report_hold_ends_with_its_guard)
{
    FakeTransport transport;
    Sender sender{STEREO_L24, {}, transport, "Out"};
    {
        auto first = sender.hold_reports();
        auto moved = std::move(first);
        EXPECT_TRUE(sender.holds_for_reports());
        auto second = sender.hold_reports();
        moved = {};
        EXPECT_TRUE(sender.holds_for_reports());  // the second still holds it
    }
    EXPECT_FALSE(sender.holds_for_reports());
}

// Each session holds the sender; IPMX is declared while any does, and its coming and going is a new session version
TEST(aes67_rtcp, the_ipmx_declaration_follows_the_sessions_and_bumps_the_version)
{
    FakeTransport transport;
    Sender sender{STEREO_L24, {}, transport, "Out"};
    (void)sender.configure(Destination{.address = GROUP.address, .port = GROUP.port});
    auto const version = [&] { return sender.session().value_or(sdp::Session{}).session_version; };
    uint64_t const plain = version();
    auto first = std::move(*RtcpSession::open_sender(sender, transport, options("a@10.0.0.2")));
    uint64_t const held = version();
    EXPECT_TRUE(held > plain);
    auto second = std::move(*RtcpSession::open_sender(sender, transport, options("b@10.0.0.2")));
    EXPECT_EQ(version(), held);
    first->close();
    EXPECT_TRUE(sender.holds_for_reports());  // the second still holds it
    second->close();
    EXPECT_FALSE(sender.holds_for_reports());
    EXPECT_TRUE(version() > held);
}

// VSF TR-10-1 Section 8.10.1: one SR every int(10 ms / ptime) packets, each before its packet and after the last one's
TEST(aes67_rtcp, sender_reports_go_before_their_packets_every_ten_milliseconds)
{
    SenderRig rig;
    rig.run(T0, 120);
    auto const& log = rig.transport.log;
    auto const position = [&](bool const rtcp, uint32_t const timestamp) {
        auto const it = std::ranges::find_if(log, [&](auto const& e) { return e.rtcp == rtcp && e.timestamp == timestamp; });
        return it - log.begin();
    };
    auto const first = static_cast<uint32_t>(rig.sender.send_state().first_index);
    size_t reports = 0;
    for (auto const& entry : log) {
        if (!entry.rtcp) {
            continue;
        }
        uint32_t const k = (entry.timestamp - first) / 480;  // ten 1 ms packets of 48 frames
        EXPECT_EQ(entry.timestamp, first + (k * 480));
        EXPECT_TRUE(position(true, entry.timestamp) < position(false, entry.timestamp));
        if (k > 0) {
            EXPECT_TRUE(position(false, entry.timestamp - 480) < position(true, entry.timestamp));
        }
        ++reports;
    }
    EXPECT_TRUE(reports >= 9);
    EXPECT_EQ(rig.session->stats().sr_sent, uint64_t{reports});
}

TEST(aes67_rtcp, a_late_session_passes_over_a_report_whose_packet_left)
{
    SenderRig rig;
    rig.run(T0, 30);
    size_t const before = rig.transport.log.size();
    for (uint64_t t = T0 + (31 * MS); t <= T0 + (60 * MS); t += MS) {
        rig.sender.tick(index_at(t), 0);
    }
    rig.session->tick(T0 + (60 * MS));
    auto const& log = rig.transport.log;
    EXPECT_TRUE(log.back().rtcp);
    EXPECT_TRUE(log.size() > before + 1);
    auto const first = static_cast<uint32_t>(rig.sender.send_state().first_index);
    EXPECT_EQ((log.back().timestamp - first) % 480, uint32_t{0});
    EXPECT_TRUE(std::ranges::none_of(log, [&](auto const& e) { return !e.rtcp && e.timestamp >= log.back().timestamp; }));
}

// VSF TR-10-1 Sections 10.1 and 10.4: an IPMX sender says so in fmtp, and names its own clock without a grandmaster
TEST(aes67_rtcp, a_held_sender_declares_ipmx_and_names_its_mac_without_a_grandmaster)
{
    FakeTransport transport;
    Sender sender{STEREO_L24, {}, transport, "Out"};
    (void)sender.configure(Destination{.address = GROUP.address, .port = GROUP.port, .payload_type = 97});
    EXPECT_FALSE(sender.session().value_or(sdp::Session{}).media.at(0).ipmx);
    auto session = std::move(*RtcpSession::open_sender(sender, transport, options("bridge@10.0.0.2")));
    auto const text = sdp::to_string(sender.session().value_or(sdp::Session{}));
    EXPECT_TRUE(text.find("a=fmtp:97 IPMX\r\n") != std::string::npos);
    EXPECT_TRUE(text.find("a=ts-refclk:localmac=00-20-FC-32-2F-40\r\n") != std::string::npos);
    sender.tick(index_at(T0), 0);
    session->tick(T0);
    EXPECT_EQ(info_block_of(transport.rtcp_out.at(0)).ts_refclk, std::string_view{"localmac=00-20-FC-32-2F-40"});
    session->close();
    EXPECT_FALSE(sender.holds_for_reports());
    auto const closed = sender.session();
    EXPECT_TRUE(closed.has_value());
    EXPECT_FALSE(closed && closed->media.at(0).ipmx);
}

// A member interface's output: its indices are that PTP's, so its SRs carry that time, whatever the reactor's clock reads
TEST(aes67_rtcp, an_sr_names_its_packets_own_time_whatever_the_session_clock_reads)
{
    constexpr uint64_t MEMBER = 1'000'000ULL * 1'000'000'000ULL;  // a timescale near 1970, as Dante's
    SenderRig rig;
    for (uint64_t t = 0; t <= 30 * MS; t += MS) {
        rig.sender.tick(index_at(MEMBER + t), 0);
        rig.session->tick(T0 + t);
    }
    auto const sr = sr_of(rig.transport.rtcp_out.at(0));
    uint64_t const first = rig.sender.send_state().first_index;
    EXPECT_EQ(sr_time(sr, MEMBER), time_of(first));
    EXPECT_EQ(sr.info.rtp_timestamp.get(), static_cast<uint32_t>(first));
}

TEST(aes67_rtcp, the_info_block_version_follows_the_senders_changes)
{
    SenderRig rig;
    rig.run(T0, 10);
    uint8_t const first = info_block_of(rig.transport.rtcp_out.at(0)).version;
    rig.sender.set_channel_order("SMPTE2110.(ST)");
    rig.run(T0 + (11 * MS), 30);
    auto const block = info_block_of(rig.transport.rtcp_out.back());
    EXPECT_EQ(block.version, static_cast<uint8_t>(first + 1));
    EXPECT_TRUE(block.pcm.has_value() && block.pcm->channel_order == std::string_view{"SMPTE2110.(ST)"});
}

TEST(aes67_rtcp, the_default_channel_order_groups_up_to_64_channels)
{
    FakeTransport transport;
    Sender stereo{STEREO_L24, {}, transport, "Two"};
    EXPECT_EQ(stereo.channel_order(), std::string{"SMPTE2110.(U02)"});
    StreamFormat wide = STEREO_L24;
    wide.channels = 136;
    Sender many{wide, {}, transport, "Many"};
    EXPECT_EQ(many.channel_order(), std::string{"SMPTE2110.(U64,U64,U08)"});
    many.set_channel_order("SMPTE2110.(51,51,ST)");
    EXPECT_EQ(many.channel_order(), std::string{"SMPTE2110.(51,51,ST)"});
}

TEST(aes67_rtcp, a_sender_not_sending_reports_nothing)
{
    FakeTransport transport;
    Sender sender{STEREO_L24, {}, transport, "Out"};
    (void)sender.configure(Destination{.address = GROUP.address, .port = GROUP.port});
    auto session = RtcpSession::open_sender(sender, transport, options("bridge@10.0.0.2"));
    EXPECT_TRUE(session.has_value());
    (*session)->tick(T0);
    (*session)->tick(T0 + (20 * MS));  // due, but the sender has not ticked since configure()
    EXPECT_TRUE(transport.rtcp_out.empty());

    Sender idle{STEREO_L24, {}, transport, "Idle"};
    EXPECT_TRUE(RtcpSession::open_sender(idle, transport, options("x")).error() == Aes67Error::not_configured);
}

TEST(aes67_rtcp, a_paused_sender_says_bye_once_and_reports_again_when_it_sends)
{
    SenderRig rig;
    rig.run(T0, 30);
    size_t const reported = rig.transport.rtcp_out.size();
    EXPECT_TRUE(reported >= 2);
    uint32_t const ssrc = rig.sender.ssrc();

    // Stopped: a BYE at the next tick, not at the report interval, then nothing.
    rig.sender.pause();
    rig.session->tick(T0 + (31 * MS));
    EXPECT_EQ(rig.transport.rtcp_out.size(), reported + 1);
    auto const bye = rtp::parse_rtcp(rig.transport.rtcp_out.back());
    EXPECT_TRUE(bye.has_value() && bye->bye.has_value() && bye->bye->contains(ssrc));
    EXPECT_FALSE(bye.has_value() && bye->sr.has_value());
    EXPECT_EQ(rig.session->stats().bye_sent, uint64_t{1});
    for (uint64_t t = T0 + (32 * MS); t <= T0 + (60 * MS); t += MS) {
        rig.session->tick(t);
    }
    EXPECT_EQ(rig.transport.rtcp_out.size(), reported + 1);

    // Sending again, under a new SSRC: its SRs, and its BYE when closed.
    rig.run(T0 + (61 * MS), 20);
    EXPECT_TRUE(rig.transport.rtcp_out.size() >= reported + 2);
    EXPECT_NE(rig.sender.ssrc(), ssrc);
    auto const compound = rtp::parse_rtcp(rig.transport.rtcp_out.back());
    auto const* const sr = compound && compound->sr ? &*compound->sr : nullptr;
    EXPECT_EQ(sr != nullptr ? sr->ssrc : 0U, rig.sender.ssrc());
    size_t const before = rig.transport.rtcp_out.size();
    rig.session->close();
    EXPECT_EQ(rig.transport.rtcp_out.size(), before + 1);
    EXPECT_EQ(rig.session->stats().bye_sent, uint64_t{1});
}

TEST(aes67_rtcp, a_large_clock_step_back_onto_the_runs_first_packet_says_no_bye)
{
    SenderRig rig;
    rig.run(T0, 40);
    SendState const before = rig.sender.send_state();
    EXPECT_TRUE(before.sent);
    EXPECT_TRUE(before.next_index > before.first_index + (uint64_t{Sender::MAX_BURST} * 48));
    // PTP steps back so far that the sender resynchronises to the packet before the run's first: the one it sends
    // leaves next_index on first_index, and the run is still sending.
    uint64_t const first = before.first_index;
    rig.sender.tick(first, 0);
    rig.session->tick(time_of(first));
    EXPECT_EQ(rig.sender.send_state().next_index, first);
    EXPECT_TRUE(rig.sender.send_state().sent);
    EXPECT_EQ(rig.session->stats().bye_sent, uint64_t{0});
}

TEST(aes67_rtcp, a_pause_and_resume_between_two_ticks_still_says_bye_for_the_old_ssrc)
{
    SenderRig rig;
    rig.run(T0, 30);
    uint32_t const old = rig.sender.ssrc();
    rig.sender.pause();
    rig.sender.tick(index_at(T0 + (31 * MS)), 0);  // resumed before the session's next tick
    uint32_t const fresh = rig.sender.ssrc();
    EXPECT_NE(fresh, old);
    size_t const before = rig.transport.rtcp_out.size();
    rig.run(T0 + (32 * MS), 30);
    EXPECT_TRUE(rig.transport.rtcp_out.size() >= before + 2);
    if (rig.transport.rtcp_out.size() < before + 2) {
        return;
    }
    auto const bye = rtp::parse_rtcp(rig.transport.rtcp_out[before]);
    EXPECT_TRUE(bye.has_value() && bye->bye.has_value() && bye->bye->contains(old));
    EXPECT_FALSE(bye.has_value() && bye->bye.has_value() && bye->bye->contains(fresh));
    auto const compound = rtp::parse_rtcp(rig.transport.rtcp_out.back());
    auto const* const sr = compound && compound->sr ? &*compound->sr : nullptr;
    EXPECT_EQ(sr != nullptr ? sr->ssrc : 0U, fresh);
    EXPECT_EQ(rig.session->stats().bye_sent, uint64_t{1});
}

TEST(aes67_rtcp, a_sender_closed_while_stopped_says_bye_only_once)
{
    SenderRig rig;
    rig.run(T0, 30);
    rig.sender.pause();
    rig.session->tick(T0 + (31 * MS));
    size_t const sent = rig.transport.rtcp_out.size();
    rig.session->close();
    EXPECT_EQ(rig.transport.rtcp_out.size(), sent);
    EXPECT_EQ(rig.session->fd(), -1);
}

TEST(aes67_rtcp, a_sender_measures_the_round_trip_from_a_report_block)
{
    SenderRig rig;
    rig.run(T0, 0);
    auto const sr = rtp::parse_rtcp(rig.transport.rtcp_out[0])->sr;
    uint32_t const lsr = rtp::lsr_of(sr->info.ntp_msw.get(), sr->info.ntp_lsw.get());

    // A receiver held the SR 2 ms and reports 1/8 lost; its RR arrives 2.5 ms after the SR.
    rtp::ReportBlock block{};
    block.ssrc = rig.sender.ssrc();
    block.set_loss(32, 7);
    block.jitter = 96U;
    block.lsr = lsr;
    block.dlsr = rtp::dlsr_from_ns(2 * MS);
    std::array<uint8_t, 128> out{};
    rtp::RtcpWriter writer{out};
    writer.receiver_report(0xACE, std::span{&block, 1}).sdes(0xACE, "listener@10.0.0.9");
    rig.transport.rtcp_in.emplace_back(writer.bytes().begin(), writer.bytes().end());
    rig.session->tick(T0 + 2'500'000);

    auto const& stats = rig.session->stats();
    EXPECT_EQ(stats.rr_received, uint64_t{1});
    EXPECT_EQ(stats.remote_ssrc, uint32_t{0xACE});
    EXPECT_EQ(stats.remote_fraction_lost, uint8_t{32});
    EXPECT_EQ(stats.remote_cumulative_lost, int32_t{7});
    EXPECT_EQ(stats.remote_jitter, uint32_t{96});
    EXPECT_EQ(stats.remote_jitter_us, uint32_t{2000});
    EXPECT_EQ(stats.remote_cname, std::string{"listener@10.0.0.9"});
    EXPECT_TRUE(stats.rtt_ns.has_value());
    EXPECT_TRUE(*stats.rtt_ns >= 500'000);
    EXPECT_TRUE(*stats.rtt_ns < 500'000 + 15'259);  // DLSR's resolution
}

TEST(aes67_rtcp, a_session_skips_its_own_reports_looped_back)
{
    SenderRig rig;
    rig.run(T0, 10);
    rig.transport.rtcp_in.push_back(rig.transport.rtcp_out[0]);
    rig.session->tick(T0 + (11 * MS));
    EXPECT_EQ(rig.session->stats().sr_received, uint64_t{0});
    rig.transport.rtcp_in.push_back({1, 2, 3, 4});
    rig.session->tick(T0 + (12 * MS));
    EXPECT_EQ(rig.session->stats().bad_packets, uint64_t{1});
}

TEST(aes67_rtcp, closing_sends_a_report_sdes_and_bye_once)
{
    SenderRig rig;
    rig.session->close();
    rig.session->close();
    EXPECT_EQ(rig.transport.rtcp_out.size(), size_t{1});
    auto const compound = rtp::parse_rtcp(rig.transport.rtcp_out[0]);
    EXPECT_TRUE(compound.has_value());
    EXPECT_EQ(compound->packets, size_t{3});
    EXPECT_TRUE(compound->rr.has_value());
    EXPECT_EQ(compound->sdes->cname, std::string_view{"bridge@10.0.0.2"});
    EXPECT_TRUE(compound->bye->contains(rig.sender.ssrc()));
    EXPECT_EQ(rig.session->fd(), -1);
}

TEST(aes67_rtcp, a_receiver_reports_loss_jitter_and_the_last_sr)
{
    FakeTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    EXPECT_TRUE(receiver.configure(stream_session(GROUP)).has_value());
    auto session = std::move(*RtcpSession::open_receiver(receiver, transport, options("bridge@10.0.0.2", 5 * MS)));
    EXPECT_TRUE(transport.opened[0].destination == (Endpoint{.address = GROUP.address, .port = 5005}));
    uint64_t const base = index_at(T0);

    // Packets 0 to 9 on time but 5 lost: 1 of the 9 expected after probation.
    for (uint16_t seq = 0; seq < 10; ++seq) {
        if (seq != 5) {
            transport.rtp_in.push_back(rtp_packet(seq, base + (uint64_t{seq} * 48)));
        }
        receiver.drain(base + (uint64_t{seq} * 48) + 48);
    }
    session->tick(T0);  // armed
    transport.rtcp_in.push_back(make_sr(0x5EED, T0 + MS));
    session->tick(T0 + (3 * MS));  // the SR arrives
    session->tick(T0 + (5 * MS));  // due

    EXPECT_EQ(session->stats().sr_received, uint64_t{1});
    EXPECT_EQ(session->stats().remote_cname, std::string{"talker@10.0.0.1"});
    EXPECT_EQ(transport.rtcp_out.size(), size_t{1});
    auto const compound = rtp::parse_rtcp(transport.rtcp_out[0]);
    EXPECT_TRUE(compound.has_value());
    EXPECT_EQ(compound->rr->ssrc, session->ssrc());
    auto const block = compound->rr->blocks.find(0x5EED);
    EXPECT_TRUE(block.has_value());
    EXPECT_EQ(block->cumulative_lost(), int32_t{1});
    EXPECT_EQ(block->fraction_lost(), uint8_t{256 / 9});
    EXPECT_EQ(block->extended_highest_seq.get(), uint32_t{9});
    EXPECT_EQ(block->jitter.get(), uint32_t{0});  // every packet arrived a packet time after its timestamp
    auto const truncated = rtp::ptp_truncated_from_ns(T0 + MS);
    EXPECT_EQ(block->lsr.get(), rtp::lsr_of(truncated.seconds, truncated.nanoseconds));
    EXPECT_EQ(block->dlsr.get(), rtp::dlsr_from_ns(2 * MS));
    EXPECT_EQ(compound->sdes->cname, std::string_view{"bridge@10.0.0.2"});
    EXPECT_EQ(session->stats().rr_sent, uint64_t{1});

    // A BYE from the sender is noted.
    std::array<uint8_t, 64> out{};
    rtp::RtcpWriter writer{out};
    writer.receiver_report(0x5EED).bye(0x5EED);
    transport.rtcp_in.emplace_back(writer.bytes().begin(), writer.bytes().end());
    session->tick(T0 + (6 * MS));
    EXPECT_EQ(session->stats().bye_received, uint64_t{1});
    EXPECT_TRUE(session->stats().remote_bye);

    // Reset: the counts go, what the remote last reported stays.
    session->reset_stats();
    EXPECT_TRUE(session->stats().sr_received == 0 && session->stats().rr_sent == 0 && session->stats().bye_received == 0);
    EXPECT_EQ(session->stats().remote_cname, std::string{"talker@10.0.0.1"});
    EXPECT_TRUE(session->stats().remote_bye && session->stats().remote_ssrc == 0x5EED);
}

TEST(aes67_rtcp, a_receiver_reads_rtcp_from_its_streams_source_alone)
{
    FakeTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    EXPECT_TRUE(receiver.configure(stream_session(GROUP)).has_value());
    std::vector<SenderReportTime> reports;
    auto opts = options("bridge@10.0.0.2", 5 * MS);
    opts.on_sender_report = [&](SenderReportTime const& report) { reports.push_back(report); };
    auto session = std::move(*RtcpSession::open_receiver(receiver, transport, std::move(opts)));
    uint64_t const base = index_at(T0);

    // Before the stream's first packet, with no source filter, no host's SR is the sender's.
    transport.rtcp_in.push_back(make_ipmx_sr(0x5EED, T0, static_cast<uint32_t>(base)));
    session->tick(T0);
    EXPECT_EQ(session->stats().sr_received, uint64_t{0});

    // Once the stream's packets come from 10.0.0.1, an SR from another host naming its SSRC is ignored.
    transport.rtp_in.push_back(rtp_packet(0, base));
    receiver.drain(base + 48);
    EXPECT_TRUE(receiver.receive_state().source == (IPv4Address{10, 0, 0, 1}));
    transport.rtcp_source = IPv4Address{10, 0, 0, 66};
    transport.rtcp_in.push_back(make_ipmx_sr(0x5EED, T0 + MS, static_cast<uint32_t>(base)));
    session->tick(T0 + MS);
    EXPECT_EQ(session->stats().sr_received, uint64_t{0});
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(session->stats().remote_sr.has_value());

    // The sender's own is read.
    transport.rtcp_source = IPv4Address{10, 0, 0, 1};
    transport.rtcp_in.push_back(make_ipmx_sr(0x5EED, T0 + (2 * MS), static_cast<uint32_t>(base)));
    session->tick(T0 + (2 * MS));
    EXPECT_EQ(session->stats().sr_received, uint64_t{1});
    EXPECT_EQ(reports.size(), size_t{1});
}

TEST(aes67_rtcp, a_receiver_with_a_source_filter_reads_its_sources_rtcp_before_the_stream)
{
    FakeTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    auto described = stream_session(GROUP);
    described.source_filter = sdp::SourceFilter{.destination = std::nullopt, .source = IPv4Address{10, 0, 0, 7}};
    EXPECT_TRUE(receiver.configure(described).has_value());
    EXPECT_TRUE(receiver.receive_state().source == (IPv4Address{10, 0, 0, 7}));
    auto session = std::move(*RtcpSession::open_receiver(receiver, transport, options("bridge@10.0.0.2", 5 * MS)));
    transport.rtcp_in.push_back(make_sr(0x5EED, T0));  // from 10.0.0.1: not the filter's
    session->tick(T0);
    EXPECT_EQ(session->stats().sr_received, uint64_t{0});
    transport.rtcp_source = IPv4Address{10, 0, 0, 7};
    transport.rtcp_in.push_back(make_sr(0x5EED, T0 + MS));
    session->tick(T0 + MS);
    EXPECT_EQ(session->stats().sr_received, uint64_t{1});
}

TEST(aes67_rtcp, a_sender_report_pairs_a_domain_index_with_its_ptp_time)
{
    // A domain 100 ppm fast, 12.345 us ahead of PTP at T0.
    mediaclock::Timebase const domain{
        .reference_ns = static_cast<int64_t>(T0),
        .domain_ns = static_cast<int64_t>(T0) + 12'345,
        .rate_offset = 100e-6,
        .running = true};
    itc::SeqlockValue<mediaclock::Timebase> timebase{domain};
    auto const clock = *mediaclock::PtpMediaClock::make(48000);
    auto domain_index = [&](uint64_t const t) {
        return clock.index_at(static_cast<uint64_t>(domain.to_domain(static_cast<int64_t>(t))));
    };
    SenderRig rig;
    rig.sender.set_sender_clock(true);
    rig.session->set_timebase(&timebase);
    auto run = [&](uint64_t const from, uint64_t const to) {
        for (uint64_t t = from; t <= to; t += MS) {
            rig.sender.tick(domain_index(t), 0);
            rig.session->tick(t);
        }
    };
    run(T0, T0 + (30 * MS));
    auto const sr = sr_of(rig.transport.rtcp_out.at(0));
    uint64_t const at = sr_time(sr, T0);
    uint64_t const index = rig.sender.send_state().first_index;  // the held first packet's
    EXPECT_EQ(sr.info.rtp_timestamp.get(), static_cast<uint32_t>(index));
    // The time field is the PTP time of that index, within the timebase's nanosecond.
    int64_t const error = domain.to_domain(static_cast<int64_t>(at)) - static_cast<int64_t>(clock.time_of(index));
    EXPECT_TRUE(error >= -1);
    EXPECT_TRUE(error <= 1);
    // So a receiver in the domain reads offset 0 from it.
    SenderReportTime const report{.rtp_timestamp = sr.info.rtp_timestamp.get(), .ptp_ns = at};
    EXPECT_TRUE(sender_clock_offset(report, domain, 48000) == std::optional<uint32_t>{0});

    auto const block = info_block_of(rig.transport.rtcp_out.at(0));
    EXPECT_EQ(block.mediaclk, std::string_view{"sender"});
    EXPECT_EQ(block.ts_refclk, std::string_view{"ptp=IEEE1588-2008:EC-46-70-FF-FE-10-FF-B0:0"});
    auto const pcm = block.pcm.value_or(rtp::IpmxPcmMediaInfo{});
    EXPECT_EQ(pcm.sampling_rate, uint32_t{48000});
    EXPECT_EQ(pcm.measured_sample_rate, uint32_t{48005});  // 48004.8
    EXPECT_EQ(block.version, rig.sender.info_block_version());

    // The measured rate moving is a new Info Block version; a domain not running sends no SR.
    timebase.store(mediaclock::Timebase{.reference_ns = static_cast<int64_t>(T0), .domain_ns = domain.domain_ns, .running = true});
    run(T0 + (31 * MS), T0 + (50 * MS));
    auto const moved = info_block_of(rig.transport.rtcp_out.back());
    EXPECT_EQ(moved.pcm.value_or(rtp::IpmxPcmMediaInfo{}).measured_sample_rate, uint32_t{48000});
    EXPECT_EQ(moved.version, static_cast<uint8_t>(block.version + 1));
    size_t const sent = rig.transport.rtcp_out.size();
    timebase.store(mediaclock::Timebase{});
    for (uint64_t t = T0 + (51 * MS); t <= T0 + (80 * MS); t += MS) {
        rig.session->tick(t);
    }
    EXPECT_EQ(rig.transport.rtcp_out.size(), sent);
}

TEST(aes67_rtcp, an_identity_timebase_leaves_the_sender_report_unchanged)
{
    itc::SeqlockValue<mediaclock::Timebase> timebase{mediaclock::Timebase{.running = true}};
    SenderRig rig;
    auto mapped = std::move(*RtcpSession::open_sender(rig.sender, rig.transport, options("bridge@10.0.0.2")));
    mapped->set_timebase(&timebase);
    for (uint64_t t = T0; t <= T0 + (30 * MS); t += MS) {
        rig.sender.tick(index_at(t), 0);
        rig.session->tick(t);
        mapped->tick(t);
    }
    auto const& out = rig.transport.rtcp_out;
    EXPECT_TRUE(out.size() >= 2);
    EXPECT_EQ(out.size() % 2, size_t{0});
    for (size_t i = 0; i + 1 < out.size(); i += 2) {
        EXPECT_TRUE(out.at(i) == out.at(i + 1));
    }
}

TEST(aes67_rtcp, a_foreign_domain_input_waits_for_a_sender_report)
{
    FakeTransport transport;
    std::vector<std::pair<uint64_t, uint32_t>> placed;
    Receiver receiver{
        STEREO_L24,
        [&](uint64_t const first, uint32_t const frames, std::span<uint8_t const>) { placed.emplace_back(first, frames); },
        transport};
    receiver.set_sender_clock(true);
    auto described = stream_session(GROUP);
    described.media[0].media_clock = sdp::MediaClock::sender();
    EXPECT_TRUE(receiver.configure(described).has_value());

    std::vector<SenderReportTime> reports;
    auto opts = options("bridge@10.0.0.2", 5 * MS);
    opts.on_sender_report = [&](SenderReportTime const& report) { reports.push_back(report); };
    auto session = std::move(*RtcpSession::open_receiver(receiver, transport, std::move(opts)));

    // The stream's clock: a domain 100 ppm slow, its RTP timestamps the domain index plus 0x12345678.
    mediaclock::Timebase const domain{
        .reference_ns = static_cast<int64_t>(T0), .domain_ns = static_cast<int64_t>(T0), .rate_offset = -100e-6, .running = true};
    auto const clock = *mediaclock::PtpMediaClock::make(48000);
    auto domain_index = [&](uint64_t const t) {
        return clock.index_at(static_cast<uint64_t>(domain.to_domain(static_cast<int64_t>(t))));
    };
    uint32_t const offset = 0x1234'5678;
    auto stream_packet = [&](uint16_t const seq, uint64_t const index) {
        auto bytes = rtp_packet(seq, index);
        rtp::RtpHeader header{};
        header.init(96, seq, static_cast<uint32_t>(index) + offset, 0x5EED);
        (void)store_unchecked(std::span{bytes}, header);
        return bytes;
    };

    // Before any sender report nothing is placed.
    uint64_t const first = domain_index(T0);
    transport.rtp_in.push_back(stream_packet(0, first));
    receiver.drain(first + 48);
    EXPECT_TRUE(placed.empty());
    EXPECT_EQ(receiver.counters().dropped, uint64_t{1});
    EXPECT_TRUE(receiver.state() == State::ready);
    EXPECT_FALSE(receiver.offset().has_value());
    EXPECT_EQ(receiver.receive_state().ssrc, uint32_t{0x5EED});  // learnt, so only its reports count

    // An SR not in the IPMX form names no PTP time: still waiting.
    session->tick(T0);
    transport.rtcp_in.push_back(make_sr(0x5EED, T0 + MS));
    session->tick(T0 + MS);
    EXPECT_EQ(session->stats().sr_received, uint64_t{1});
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(session->stats().remote_sr.has_value());

    // The first IPMX SR, a third of a sample off the sample grid, gives the offset.
    uint64_t const sr_ptp = T0 + (2 * MS) + 7'000;
    uint32_t const sr_rtp = static_cast<uint32_t>(domain_index(sr_ptp)) + offset;
    transport.rtcp_in.push_back(make_ipmx_sr(0x5EED, sr_ptp, sr_rtp));
    session->tick(T0 + (2 * MS) + 300'000);
    EXPECT_EQ(reports.size(), size_t{1});
    EXPECT_TRUE(
        reports.at(0) == (SenderReportTime{.rtp_timestamp = sr_rtp, .ptp_ns = sr_ptp, .arrival_ns = T0 + (2 * MS) + 300'000}));
    EXPECT_TRUE(session->stats().remote_sr == reports.at(0));
    auto const parsed = sender_clock_offset(reports.at(0), domain, 48000);
    EXPECT_TRUE(parsed == std::optional<uint32_t>{offset});
    receiver.set_offset(*parsed);
    EXPECT_TRUE(receiver.offset() == std::optional<uint32_t>{offset});

    // Then packets are placed at the domain's index.
    uint64_t const next = first + (3 * 48);
    transport.rtp_in.push_back(stream_packet(3, next));
    receiver.drain(next + 48);
    EXPECT_EQ(placed.size(), size_t{1});
    EXPECT_TRUE(placed.at(0) == (std::pair<uint64_t, uint32_t>{next, 48}));
    EXPECT_TRUE(receiver.state() == State::running);

    // A new source after a second's silence has a timestamp base of its own: it waits again.
    uint64_t const later = next + 48'000;
    transport.rtp_in.push_back(rtp_packet(900, later, 0xBEEF));
    transport.rtp_in.push_back(rtp_packet(901, later + 48, 0xBEEF));
    receiver.drain(later + 96);
    EXPECT_FALSE(receiver.offset().has_value());
    EXPECT_EQ(placed.size(), size_t{1});
    EXPECT_EQ(receiver.receive_state().ssrc, uint32_t{0xBEEF});
}

TEST(aes67_rtcp, receiver_jitter_follows_rfc_3550_appendix_a8)
{
    FakeTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    EXPECT_TRUE(receiver.configure(stream_session(GROUP)).has_value());
    uint64_t const base = index_at(T0);
    // Arrival offsets (samples past the timestamp): the transit differences D drive J.
    std::array<uint32_t, 6> const delay{48, 48, 144, 48, 54, 48};
    uint32_t expected_x16 = 0;
    for (size_t i = 0; i < delay.size(); ++i) {
        uint64_t const ts = base + (i * 48);
        transport.rtp_in.push_back(rtp_packet(static_cast<uint16_t>(i), ts));
        receiver.drain(ts + delay[i]);
        if (i > 0) {
            int64_t const d = static_cast<int64_t>(delay[i]) - static_cast<int64_t>(delay[i - 1]);
            expected_x16 += static_cast<uint32_t>(d < 0 ? -d : d) - ((expected_x16 + 8) >> 4U);
        }
    }
    EXPECT_EQ(receiver.receive_state().jitter_x16, expected_x16);
    EXPECT_TRUE(expected_x16 > 0);
}

TEST(aes67_rtcp, packets_drained_together_add_no_jitter)
{
    FakeTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    EXPECT_TRUE(receiver.configure(stream_session(GROUP)).has_value());
    uint64_t const base = index_at(T0);
    // Four packets a drain, each drain a packet time after the last packet's timestamp.
    for (uint16_t drain = 0; drain < 4; ++drain) {
        for (uint16_t k = 0; k < 4; ++k) {
            auto const seq = static_cast<uint16_t>((drain * 4) + k);
            transport.rtp_in.push_back(rtp_packet(seq, base + (uint64_t{seq} * 48)));
        }
        receiver.drain(base + (uint64_t{drain} * 4 * 48) + (4 * 48));
    }
    EXPECT_EQ(receiver.receive_state().jitter_x16, uint32_t{0});
}

TEST(aes67_rtcp, receiver_rtcp_follows_the_sdp)
{
    FakeTransport transport;
    Receiver receiver{STEREO_L24, {}, transport};
    EXPECT_TRUE(receiver.rtcp_options().error() == Aes67Error::not_configured);

    auto session = stream_session(GROUP);
    session.media[0].rtcp = sdp::RtcpAttribute{.port = 6000, .address = std::nullopt};
    EXPECT_TRUE(receiver.configure(session).has_value());
    auto options = receiver.rtcp_options(34);
    EXPECT_TRUE(options->destination == (Endpoint{.address = GROUP.address, .port = 6000}));
    EXPECT_EQ(options->dscp, uint8_t{34});

    session.media[0].rtcp_mux = true;
    EXPECT_TRUE(receiver.configure(session).has_value());  // the stream is received all the same
    EXPECT_TRUE(receiver.rtcp_options().error() == Aes67Error::rtcp_multiplexed);

    // Unicast: reports arrive here and go to the sender at the same port.
    Endpoint const unicast{.address = IPv4Address{10, 0, 0, 2}, .port = 5004};
    EXPECT_TRUE(receiver.configure(stream_session(unicast)).has_value());
    options = receiver.rtcp_options();
    EXPECT_TRUE(options->local == (Endpoint{.address = unicast.address, .port = 5005}));
    EXPECT_TRUE(options->destination == (Endpoint{.address = IPv4Address{10, 0, 0, 1}, .port = 5005}));
    // RFC 3605's address is where reports go, for a multicast stream too.
    auto pointed = stream_session(GROUP);
    pointed.media[0].rtcp = sdp::RtcpAttribute{.port = 5007, .address = IPv4Address{10, 0, 0, 9}};
    EXPECT_TRUE(receiver.configure(pointed).has_value());
    options = receiver.rtcp_options();
    EXPECT_TRUE(options->destination == (Endpoint{.address = IPv4Address{10, 0, 0, 9}, .port = 5007}));
    EXPECT_TRUE(options->local == (Endpoint{.address = IPv4Address{}, .port = 5007}));

    auto nameless = stream_session(unicast);
    nameless.origin_address = "talker.local";
    EXPECT_TRUE(receiver.configure(nameless).has_value());
    EXPECT_TRUE(receiver.rtcp_options().error() == Aes67Error::no_rtcp_peer);
}

namespace {

/// A multicast group of this process's own (239.(128-191).x.y from the pid).
auto own_group() -> IPv4Address
{
    auto const pid = static_cast<uint32_t>(::getpid());
    return IPv4Address{
        239, static_cast<uint8_t>(128 + ((pid >> 16U) % 64)), static_cast<uint8_t>(pid >> 8U), static_cast<uint8_t>(pid)};
}

}  // namespace

TEST(aes67_rtcp, sr_rr_and_bye_round_trip_over_udp_loopback)
{
    auto transport = UdpTransport::open("lo");
    EXPECT_TRUE(transport.has_value());
    Endpoint const group{.address = own_group(), .port = 45300};
    Sender sender{STEREO_L24, {}, **transport, "Loop"};
    sender.set_send_offset(0);
    EXPECT_TRUE(sender.configure(Destination{.address = group.address, .port = group.port, .ttl = 1}).has_value());
    Receiver receiver{STEREO_L24, {}, **transport};
    auto described = *sender.session();
    described.media[0].source_filter.reset();  // the kernel may pick a routed source, not lo's
    if (!receiver.configure(described)) {
        std::println("  skipped: cannot join a group on lo");
        return;
    }
    auto talker = RtcpSession::open_sender(sender, **transport, options("talker@127.0.0.1", 20 * MS));
    auto listener = RtcpSession::open_receiver(receiver, **transport, options("listener@127.0.0.1", 20 * MS));
    if (!talker || !listener) {
        std::println("  skipped: cannot open RTCP on lo");
        return;
    }
    // A PTP clock that follows the host's, so delays are real; 200 ms of 1 ms steps.
    auto const start = std::chrono::steady_clock::now();
    auto ptp_now = [&] {
        return T0 +
            static_cast<uint64_t>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    };
    for (int step = 0; step < 200; ++step) {
        uint64_t const now = ptp_now();
        sender.tick(index_at(now), 0);
        receiver.drain(index_at(now));
        (*talker)->on_ready(now);
        (*listener)->on_ready(now);
        (*talker)->tick(now);
        (*listener)->tick(now);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if ((*listener)->stats().sr_received == 0) {
        std::println("  skipped: the host does not route multicast on lo");
        return;
    }
    auto const& received = (*listener)->stats();
    EXPECT_EQ(received.remote_cname, std::string{"talker@127.0.0.1"});
    EXPECT_EQ(received.remote_ssrc, sender.ssrc());
    EXPECT_TRUE((*listener)->stats().rr_sent > 0);
    auto const& reported = (*talker)->stats();
    EXPECT_TRUE(reported.sr_sent > 0);
    EXPECT_TRUE(reported.rr_received > 0);
    EXPECT_EQ(reported.remote_ssrc, (*listener)->ssrc());
    EXPECT_EQ(reported.remote_cname, std::string{"listener@127.0.0.1"});
    EXPECT_EQ(reported.remote_cumulative_lost, int32_t{0});
    EXPECT_TRUE(reported.rtt_ns.has_value());
    EXPECT_TRUE(*reported.rtt_ns < 50 * MS);

    // The listener leaves: the talker receives its BYE.
    (*listener)->close();
    for (int tries = 0; tries < 100 && (*talker)->stats().bye_received == 0; ++tries) {
        (*talker)->on_ready(ptp_now());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ((*talker)->stats().bye_received, uint64_t{1});
    EXPECT_TRUE((*talker)->stats().remote_bye);
}

TEST_MAIN(statusbar_aes67, aes67_rtcp_test)
