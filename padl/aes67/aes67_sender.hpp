#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An AES67 sender: a FrameProvider's frames by sample index, sent as RTP on the sample-index grid, each departing at
/// its tick's time, and the stream's SDP; under an RTCP session an IPMX sender (VSF TR-10-1 Section 8.10.1).
/// configure(), clear(), the setters and session() allocate; tick() and the accessors are real-time safe.

#include "padl/aes67/aes67_packet_schedule.hpp"
#include "padl/aes67/aes67_stream.hpp"
#include "padl/aes67/aes67_transport.hpp"
#include "padl/sdp/sdp_session.hpp"
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/status/status.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace padl::aes67 {

class Sender
{
  public:
    /// Packets one tick sends at most; further behind, the sender skips ahead.
    static constexpr uint32_t MAX_BURST = PacketSchedule::MAX_BURST;
    /// The largest RTP payload sent: 1440 octets, AES67-2023 Clause 6.3 (no fragmentation in a
    /// 1500-octet MTU, over IPv4 or IPv6).
    static constexpr uint32_t MAX_PAYLOAD = 1440;
    /// Session ids and versions are kept to 63 bits: positive in any signed reading.
    static constexpr uint64_t SESSION_ID_MASK = 0x7FFF'FFFF'FFFF'FFFFULL;
    /// How far ahead a held run's first packet is placed, in microseconds: time for the reactor to send its SR.
    static constexpr uint32_t FIRST_REPORT_LEAD_US = PacketSchedule::FIRST_REPORT_LEAD_US;

    /// `name` is the session name (s=) of session().
    Sender(StreamFormat const& format, FrameProvider provider, Transport& transport, std::string name);

    /// Send to `destination`, replacing any before.
    [[nodiscard]] auto configure(Destination const& destination) -> statusbar::Status;
    /// Send to `session.media[0]`'s connection, port and the payload type of its
    /// offered format with the sender's encoding; channels and rate must be the sender's.
    [[nodiscard]] auto configure(sdp::Session const& session) -> statusbar::Status;
    /// Stop sending.
    void clear() noexcept;

    /// The send offset in frames: a packet is sent once its last frame is this far
    /// behind now. Default: default_send_offset(). Any thread, real-time safe; the next tick adopts it.
    void set_send_offset(uint32_t const frames) noexcept { send_offset_.store(frames, std::memory_order_relaxed); }
    [[nodiscard]] auto send_offset() const noexcept -> uint32_t { return send_offset_.load(std::memory_order_relaxed); }
    /// One 125 us tick, rounded up (6 frames at 48 kHz): for a provider whose frames are there by their index.
    [[nodiscard]] static constexpr auto default_send_offset(StreamFormat const& format) noexcept -> uint32_t
    {
        return (format.sample_rate + 7999) / 8000;
    }

    /// Where the packet grid starts, in frames (modulo frames_per_packet):
    /// packets carry [k * fpp + phase, (k + 1) * fpp + phase). Call before
    /// configure(), off the real-time thread.
    void set_packet_phase(uint32_t const frames) noexcept { schedule_.set_phase(frames); }
    [[nodiscard]] auto packet_phase() const noexcept -> uint32_t { return schedule_.phase(); }
    /// The phase that staggers the `k`th sender: (k mod 8) 125 us ticks modulo
    /// the packet (0, 6, ... 42 frames for 1 ms at 48 kHz).
    [[nodiscard]] static constexpr auto stagger_phase(StreamFormat const& format, uint32_t const k) noexcept -> uint32_t
    {
        if (format.frames_per_packet == 0) {
            return 0;
        }
        uint32_t const tick = (format.sample_rate + 7999) / 8000;
        return ((k % 8) * tick) % format.frames_per_packet;
    }

    /// The SDP session id (o=), stable for the same stream across restarts (RFC 8866 Section 5.2). 0, the
    /// default: the first configure() uses stable_session_id() of the origin address and the name.
    /// Call before configure(), off the real-time thread.
    void set_session_id(uint64_t const id) noexcept { session_id_ = id & SESSION_ID_MASK; }
    [[nodiscard]] auto session_id() const noexcept -> uint64_t { return session_id_; }
    /// A session id from `key` (FNV-1a), the same for the same key, positive
    /// in any signed 64-bit reading and never 0.
    [[nodiscard]] static constexpr auto stable_session_id(std::string_view const key) noexcept -> uint64_t
    {
        uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
        for (char const c : key) {
            hash = (hash ^ static_cast<uint8_t>(c)) * 0x0000'0100'0000'01B3ULL;
        }
        hash &= SESSION_ID_MASK;
        return hash == 0 ? 1 : hash;
    }

    /// The PTP grandmaster ts-refclk names; nullopt until it is known, and ts-refclk then names the interface's
    /// MAC (references()). A change bumps the session version (o=), as configure() does.
    void set_grandmaster(std::optional<Grandmaster> const& grandmaster);

    [[nodiscard]] auto grandmaster() const noexcept -> std::optional<Grandmaster> const& { return grandmaster_; }
    /// What session() and the Info Block put in ts-refclk: the grandmaster's references, else `localmac=` with
    /// Transport::local_mac() (VSF TR-10-1 Section 10.4: no common reference clock), else none.
    [[nodiscard]] auto references() const -> sdp::ReferenceClocks;

    /// Announce `mediaclk:sender` (true: the index tick() is given is the sender's own clock, RFC 7273
    /// Section 5) instead of `direct=0` (false, the default); a change bumps the session and Info Block
    /// versions. ts-refclk still names the grandmaster. Off the real-time thread.
    void set_sender_clock(bool sender);
    [[nodiscard]] auto sender_clock() const noexcept -> bool { return sender_clock_; }
    /// The media clock session() and the Info Block announce.
    [[nodiscard]] auto media_clock() const noexcept -> sdp::MediaClock
    {
        return sender_clock_ ? sdp::MediaClock::sender() : sdp::MediaClock::direct(0);
    }
    /// The RAVENNA `a=clock-domain:PTPv2` session() writes beside ts-refclk: the grandmaster's IEEE 1588
    /// domain; nullopt without one, or for IEEE 802.1AS.
    [[nodiscard]] auto clock_domain() const noexcept -> std::optional<uint8_t>
    {
        bool const ptpv2 = grandmaster_ && grandmaster_->version != sdp::PtpVersion::ieee802_1as;
        return ptpv2 ? std::optional{grandmaster_->domain} : std::nullopt;
    }

    /// The session name (s=) of session(), the constructor's until changed; a new name bumps the
    /// session version. Off the real-time thread.
    void set_name(std::string name);
    [[nodiscard]] auto name() const noexcept -> std::string const& { return name_; }
    /// The session-level attributes session() writes after its own, in order (sdp::Attribute); none until set.
    /// A change bumps the session version. Off the real-time thread.
    void set_attributes(std::vector<sdp::Attribute> attributes);
    [[nodiscard]] auto attributes() const noexcept -> std::vector<sdp::Attribute> const& { return attributes_; }

    /// Adopt `previous`'s send offset, grandmaster, media clock, name, attributes, SDP session id and version, DSCP, channel
    /// order, ST 2110 form and Info Block version, so this sender's next configure() announces the same session one version on.
    /// Call before configure(), off the real-time thread.
    void continue_from(Sender const& previous);

    /// The channel-order an IPMX Info Block names (ST 2110-30 Section 6.2.2), e.g. "SMPTE2110.(ST)";
    /// empty, the default, gives undefined groups of up to 64: "SMPTE2110.(U64,U64,U08)". Off the real-time thread.
    void set_channel_order(std::string order);
    [[nodiscard]] auto channel_order() const -> std::string;
    /// Describe the stream as ST 2110-30 Section 6.2 does: session() adds the channel order (`a=fmtp:<pt>
    /// channel-order=`) and `a=maxptime`, the packet time; off, the default, it has AES67's attributes alone.
    /// A change bumps the session version. Off the real-time thread.
    void set_st2110(bool st2110);
    [[nodiscard]] auto st2110() const noexcept -> bool { return st2110_; }
    /// The IPMX Info Block's version (TR-10-1 Section 8.7): one on with each configure(), grandmaster
    /// or channel-order change, mod 256.
    [[nodiscard]] auto info_block_version() const noexcept -> uint8_t { return info_block_version_; }

    /// The DSCP the stream and its RTCP are marked with (AES67-2023 Clause 6.2), DSCP_MEDIA until set.
    /// Call before configure(), off the real-time thread.
    void set_dscp(uint8_t const dscp) noexcept { dscp_ = dscp; }
    [[nodiscard]] auto dscp() const noexcept -> uint8_t { return dscp_; }

    /// Tag the stream's packets with `vlan` (PCP and VID), or none (nullopt): from the next packet when
    /// configured, the run, its sequence and timestamps going on; kept for the next configure(). Off the
    /// real-time thread. Fails, keeping the tag before, when the transport cannot tag (SendPort::set_vlan()).
    [[nodiscard]] auto set_vlan(std::optional<statusbar::ieee::VlanTag> const& vlan) noexcept -> statusbar::Status;
    [[nodiscard]] auto vlan() const noexcept -> std::optional<statusbar::ieee::VlanTag> const& { return vlan_; }

    /// Where this stream's RTCP goes and how it is marked (AES67-2023 Clause 6.2: as the stream),
    /// or nullopt when not configured.
    [[nodiscard]] auto rtcp_options() const noexcept -> std::optional<RtcpOptions>;
    /// The latest SendState, published by tick(). Any thread.
    [[nodiscard]] auto send_state() const noexcept -> SendState { return send_state_.load(); }

    /// An ipmx RTCP session's hold on the sender, held as long as the guard lives (moved, it goes with it). While any
    /// is held, each run's first packet goes FIRST_REPORT_LEAD_US after the run starts, time for its SR (VSF TR-10-1
    /// Section 8.10.1), and session() declares IPMX. Made and ended on the thread that configures.
    class ReportHold
    {
      public:
        ReportHold() noexcept = default;
        ~ReportHold();
        ReportHold(ReportHold const&) = delete;
        auto operator=(ReportHold const&) -> ReportHold& = delete;
        ReportHold(ReportHold&& other) noexcept;
        auto operator=(ReportHold&& other) noexcept -> ReportHold&;

      private:
        friend class Sender;
        explicit ReportHold(Sender& sender) noexcept;
        Sender* sender_{nullptr};
    };

    /// A new hold for an ipmx RTCP session; the first one held bumps the session version, as the last one ending does.
    [[nodiscard]] auto hold_reports() -> ReportHold;
    /// Whether any ReportHold is held: session() declares IPMX. Any thread.
    [[nodiscard]] auto holds_for_reports() const noexcept -> bool { return holds_.load(std::memory_order_relaxed) != 0; }

    /// Send every packet due by `now_index` (the media clock's index at the current PTP time), each
    /// handed to the transport departing at `tick_ns`, this tick's time in the ns the sending NIC's
    /// PHC reads (0: untimed; SendPort::send_rendered_at); a frame port's due frames are written at their indices.
    void tick(uint64_t now_index, uint64_t tick_ns) noexcept;
    /// Send nothing until the next tick(), which starts a new run on the index grid with a new SSRC and
    /// sequence (RFC 3550 Section 8): send_state() is cleared (the RTCP session says BYE), state() reads
    /// idle, or error while Transport::send_error() (or a frame port's error()) says so. Real-time safe; the ticking
    /// thread only.
    void pause() noexcept;

    /// The stream's SDP, or nullopt when not configured.
    [[nodiscard]] auto session() const -> std::optional<sdp::Session>;

    [[nodiscard]] auto format() const noexcept -> StreamFormat const& { return format_; }
    [[nodiscard]] auto state() const noexcept -> State { return state_; }
    [[nodiscard]] auto counters() const noexcept -> Counters const& { return counters_; }
    /// Every counter to zero; the run, its SSRC and sequence go on. The ticking thread.
    void reset_counters() noexcept
    {
        counters_ = {};
        lost_elsewhere_ = 0;
    }
    /// The part of counters().lost found where the packets are sent rather than in the provider's frames: a frame port's
    /// or a gathering packet port's missing frames (SendProgress::lost) and a resend's skipped ones. The ticking thread.
    [[nodiscard]] auto lost_elsewhere() const noexcept -> uint64_t { return lost_elsewhere_; }
    /// Why the transport refused the last configure(), or why it cannot send (Transport::send_error(), else a frame
    /// port's FrameSend::error(), seen by the last tick), for State::error.
    [[nodiscard]] auto error() const noexcept -> std::error_code { return error_; }
    [[nodiscard]] auto configured() const noexcept -> bool { return port_ != nullptr; }
    /// The open port as a frame port (SendPort::frames()), or nullptr: a packet port, or none; valid from configure()
    /// until clear(). The thread that configures the sender.
    [[nodiscard]] auto frame_port() noexcept -> FrameSend* { return frames_; }
    /// The open port sends `routes`' channels from frames other ports of its device hold (FrameSend::gather_from() for a
    /// frame port, SendPort::gather_from() for a packet port); empty: from the provider again. With no port open: success
    /// for empty, not_connected otherwise. The thread that configures the sender.
    [[nodiscard]] auto gather_from(std::span<FrameRoute const> routes) -> statusbar::Status;
    /// Whether its port sends the packets rendered for it now (SendPort::sends_rendered()); true with none. The ticking
    /// thread.
    [[nodiscard]] auto sends_rendered() const noexcept -> bool { return port_ == nullptr || port_->sends_rendered(); }
    /// The current run's SSRC (a resumed run draws a new one on the ticking thread). Any thread.
    [[nodiscard]] auto ssrc() const noexcept -> uint32_t { return schedule_.ssrc(); }

  private:
    StreamFormat format_;
    FrameProvider provider_;
    Transport& transport_;
    std::string name_;
    std::vector<sdp::Attribute> attributes_;
    /// One version on (the first: the wall clock's seconds, one on).
    void bump_session_version();
    /// The frames a frame port, or a gathering packet port, found missing in its run (SendProgress::lost) since the last
    /// reading, counted lost; a reading behind the last (a page older than one already read) counts nothing.
    void count_port_lost(SendProgress const& progress) noexcept;
    /// Fill `payload` with the provider's `frames` frames from `first_index`, or
    /// silence when it has none; what the provider said.
    [[nodiscard]] auto render_payload(uint64_t first_index, uint32_t frames, std::span<uint8_t> payload) noexcept -> Supply;

    std::unique_ptr<SendPort> port_;
    FrameSend* frames_{nullptr};         ///< port_ as a frame port, or nullptr
    SendProgress port_lost_{};           ///< the run and its SendProgress::lost as counters_.lost last counted them
    uint64_t lost_elsewhere_{0};         ///< lost_elsewhere()
    std::atomic<bool> gathered_{false};  ///< a packet port offered routes since configure(): its progress() and resends read
    Destination destination_;
    std::vector<uint8_t> payload_;  ///< staging, only for a packet the transport had no TX buffer for

    /// clear(), then open the transport for send_plan(`destination`) (Transport::open_send_stream()); on failure
    /// State::error and error().
    [[nodiscard]] auto open_port(Destination const& destination) -> statusbar::Status;
    /// The plan of this sender's stream to `destination`.
    [[nodiscard]] auto send_plan(Destination const& destination) const noexcept -> SendPlan;
    /// Why the stream cannot be sent: the transport's send_error(), else a frame port's error(); none when it can.
    [[nodiscard]] auto stream_error() const noexcept -> std::error_code
    {
        auto const error = transport_.send_error();
        return error || frames_ == nullptr ? error : frames_->error();
    }
    /// Adopts `port` as the stream's, and a frame port's start lead.
    void adopt_port(std::unique_ptr<SendPort> port) noexcept;
    /// A new run: the staging buffer sized, a random SSRC, generator seed and sequence, SR counts from zero.
    void seed_run();
    /// A new session version, and the session id from the origin and name when none was set.
    void renew_session();
    /// Ready, not paused or started, with nothing published yet.
    void become_ready() noexcept;
    /// The newest packet due at `now_index` (PacketSchedule::newest_due()), skipped frames counted lost and a held run's
    /// placement published; nullopt when none is to be sent.
    [[nodiscard]] auto newest_due(uint64_t now_index) noexcept -> std::optional<uint64_t>;
    /// Publishes the held run's first packet, placed at PacketSchedule::held(), and starts a frame port's run there.
    /// Not inlined: rare, and off newest_due()'s path.
    [[gnu::noinline]] void place_held() noexcept;
    /// Sends the packets from the schedule's next index up to `newest`, then flushes them.
    void send_until(uint64_t newest) noexcept;
    /// Renders and queues the packet at the schedule's next index; true when the transport accepted it.
    [[nodiscard]] auto send_packet(rtp::RtpHeader const& header) noexcept -> bool;
    /// A gathering packet port's resend_from() followed: the schedule back there, at most a burst behind `newest`, and
    /// what went nowhere no longer counted sent. Not inlined: rare.
    [[gnu::noinline]] void resend_if_asked(uint64_t newest) noexcept;
    /// A rewind's counts applied: what went nowhere uncounted as sent, what is left behind lost, what goes or went no
    /// longer lost.
    void count_rewound(PacketSchedule::Rewound const& rewound) noexcept;
    /// `packets` a resync skipped that its port sent elsewhere counted sent, in the SR's counts too.
    void count_delivered(uint32_t packets) noexcept;
    /// `packets` sent no longer counted sent, nor in the SR's counts.
    void uncount_sent(uint32_t packets) noexcept;
    /// Flushes the `queued` packets and counts what went out.
    void flush(uint32_t queued, uint32_t run_ssrc) noexcept;
    /// Adds `packets` sent to the SR's counts and publishes them.
    void report_sent(uint32_t packets, uint32_t run_ssrc) noexcept;

    // The frame path (SendPort::frames()): the frames written at their indices, the port's run sending them.

    /// tick() on the frame path: the run started or re-placed, the send offset followed, the due packets written.
    void tick_frames(uint64_t now_index) noexcept;
    /// Starts the frame port's run, its first packet at `first_index`, stopping the one before.
    void start_run(uint64_t first_index) noexcept;
    /// Stops the frame port's run, if one was started.
    void stop_run() noexcept;
    /// Tells a packet port its send offset at the first tick and at each change (SendPort::set_send_offset()).
    void tell_send_offset() noexcept;
    /// Passes a changed send offset on to the frame port's run.
    void follow_send_offset() noexcept;
    /// Writes the packets from the schedule's next index up to `newest` to the frame port, and counts them.
    void write_until(uint64_t newest) noexcept;
    /// Renders and commits the packet at the schedule's next index into the port; true when the port accepted it.
    [[nodiscard]] auto write_packet() noexcept -> bool;
    /// Copies the packet at `first_index` from the staging buffer into the port, piece by piece where its storage
    /// wraps; false when it has no room.
    [[nodiscard]] auto copy_staged(uint64_t first_index) noexcept -> bool;
    /// Counts `packets` written and publishes the port's progress for the SR.
    void count_written(uint32_t packets) noexcept;
    /// The port's progress when it is of the run last started, else that run's before its first packet has gone.
    [[nodiscard]] auto run_progress() const noexcept -> SendProgress;
    /// channel_order() when it names the format's channels (else none: an order for other channels misleads).
    [[nodiscard]] auto described_channel_order() const -> std::string;
    /// The SDP media section this sender describes.
    [[nodiscard]] auto media_description(statusbar::ip::IPv4Address const& origin, bool multicast) const -> sdp::Media;

    PacketSchedule schedule_;
    bool paused_{false};  ///< pause() since the last tick()
    bool sender_clock_{false};
    uint8_t dscp_{rtp::FrameAddressing::DSCP_MEDIA};
    State state_{State::idle};
    std::string channel_order_;
    bool st2110_{false};
    uint8_t info_block_version_{0};
    /// Since the run began: the SR's counts (real-time thread).
    struct RtcpCounts
    {
        uint32_t packets{0};
        uint32_t octets{0};
        bool sent{false};  ///< a packet of the run has gone (SendState::sent)
    };
    RtcpCounts rtcp_counts_{};
    statusbar::itc::SeqlockValue<SendState> send_state_;  ///< real-time -> RTCP session
    uint64_t session_id_{0};
    uint64_t session_version_{0};
    std::optional<Grandmaster> grandmaster_;
    uint64_t tick_ns_{0};  ///< this tick's time: the departure of what it sends
    std::atomic<uint32_t> send_offset_;
    uint32_t run_send_offset_{0};            ///< the send offset the frame port's run was given (real-time thread)
    uint32_t port_send_offset_{UINT32_MAX};  ///< the send offset a packet port was last told (real-time thread)
    uint64_t run_first_index_{0};            ///< the first index the frame port's run was given (real-time thread)
    bool run_started_{false};                ///< the frame port has a run (real-time thread)
    std::optional<statusbar::ieee::VlanTag> vlan_;
    std::atomic<uint32_t> holds_{0};  ///< the ReportHolds held: real-time reads wait-free
    /// One more ReportHold held (`held`) or one fewer, the session version bumped when holds_for_reports() changes.
    void count_hold(bool held);
    Counters counters_;
    std::error_code error_;
};

}  // namespace padl::aes67
