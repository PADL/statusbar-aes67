#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An RTCP session beside one AES67 stream (RFC 3550 Section 6): a sender's SRs in the IPMX form and on its schedule
/// (VSF TR-10-1 Sections 8.7 and 8.10.1) with SDES CNAME, or a receiver's RR with one report block; BYE on close.
/// Reactor thread only, times in PTP ns.

#include "padl/aes67/aes67_receiver.hpp"
#include "padl/aes67/aes67_sender.hpp"
#include "padl/aes67/aes67_transport.hpp"
#include "padl/mediaclock/mediaclock_ptp_clock.hpp"
#include "padl/mediaclock/mediaclock_timebase.hpp"
#include "padl/rtp/rtcp_interval.hpp"
#include "padl/rtp/rtcp_packet.hpp"
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace padl::aes67 {

/// When a receiver's next report goes: the cadence strategy.
class RtcpCadence
{
  public:
    virtual ~RtcpCadence() = default;
    /// The delay to the next report, in ns: `initial` for the first after opening.
    [[nodiscard]] virtual auto next_interval_ns(bool initial) noexcept -> uint64_t = 0;
};

/// RFC 3550 Section 6.3.1's randomised interval (rtp::rtcp_interval()) with its own generator.
class Rfc3550Interval final : public RtcpCadence
{
  public:
    Rfc3550Interval(rtp::RtcpIntervalParams const& params, uint32_t seed) noexcept
        : params_{params}
        , rng_{seed}
    {}

    [[nodiscard]] auto next_interval_ns(bool const initial) noexcept -> uint64_t override
    {
        params_.initial = initial;
        return rtp::rtcp_interval(params_, rng_);
    }

  private:
    rtp::RtcpIntervalParams params_;
    std::minstd_rand rng_;
};

/// A sender report's pair (RFC 3550 Section 6.4.1): an RTP timestamp and the sender's Internal Clock time it names, from
/// an SR in the IPMX form (VSF TR-10-1 Section 8.7), and when the SR arrived (PTP ns). That time is PTP time only when
/// the sender's ts-refclk names a PTP grandmaster.
struct SenderReportTime
{
    uint32_t rtp_timestamp{0};
    uint64_t ptp_ns{0};
    uint64_t arrival_ns{0};

    auto operator==(SenderReportTime const&) const noexcept -> bool = default;
};

/// Called on the reactor with each of the stream's sender reports that carries the pair, from inside
/// RtcpSession::on_ready(); it must not close or destroy its session.
using SenderReportHandler = statusbar::sg14::inplace_function<void(SenderReportTime const&), 32>;

/// The RTP offset of a sender-clocked stream (RFC 7273 Section 5) from one of its reports: the RTP
/// timestamp less the index at the PTP time named, mapped through `timebase` and rounded to the
/// nearest sample. nullopt for a rate PtpMediaClock does not support or a time before the epoch.
[[nodiscard]] auto sender_clock_offset(
    SenderReportTime const& report, mediaclock::Timebase const& timebase, uint32_t sample_rate) noexcept -> std::optional<uint32_t>;

/// What a session has seen and done. "Remote" is the other end: for a sender, the latest report
/// block about its stream (any receiver's); for a receiver, the stream's sender.
struct RtcpStats
{
    uint64_t sr_sent{0};
    uint64_t rr_sent{0};
    uint64_t bye_sent{0};             ///< a sender's, each for an SSRC that stopped sending (Sender::pause())
    uint64_t sr_received{0};          ///< the stream's (receiver) or other senders' (sender)
    uint64_t rr_received{0};          ///< RRs from others, whatever they report on
    uint64_t bye_received{0};         ///< BYEs naming the remote
    uint64_t bad_packets{0};          ///< datagrams parse_rtcp() refused
    uint64_t send_errors{0};          ///< reports the port refused
    uint8_t remote_fraction_lost{0};  ///< 1/256ths, over the reporter's last interval
    int32_t remote_cumulative_lost{0};
    uint32_t remote_jitter{0};       ///< timestamp units
    uint32_t remote_jitter_us{0};    ///< the same in microseconds
    std::optional<uint64_t> rtt_ns;  ///< sender: from the latest report block echoing one of its SRs
    uint32_t remote_ssrc{0};
    std::string remote_cname;
    bool remote_bye{false};
    std::optional<SenderReportTime> remote_sr;  ///< receiver: the stream's latest IPMX-form SR
};

/// When a sender's SRs go.
enum class SenderReports : uint8_t
{
    rfc3550,  ///< RFC 3550 Section 6.3.1's randomised interval, about a second (SENDER_INTERVAL_MS); no first-packet hold
    ipmx,     ///< VSF TR-10-1 Section 8.10.1's: the first packet's, then every 10 ms of packets; the SDP declares IPMX
};

/// What a session is opened with.
struct RtcpSessionOptions
{
    std::string cname;  ///< RFC 3550 Section 6.5.1: "<identity>@<address>", at most 255 bytes
    /// A receiver's RTCP DSCP: what it would send a similar stream with (AES67-2023 Clause 6.2).
    /// A sender's is its stream's.
    uint8_t dscp{rtp::FrameAddressing::DSCP_MEDIA};
    SenderReports reports{SenderReports::rfc3550};  ///< a sender's schedule
    std::unique_ptr<RtcpCadence> cadence;           ///< rfc3550's and a receiver's; null: Rfc3550Interval
    SenderReportHandler on_sender_report;           ///< receiver: each new RtcpStats::remote_sr; may be empty
};

class RtcpSession
{
  public:
    /// RFC 3550 Section 6.2's fixed minimum, which a receiver (not an active sender) keeps.
    static constexpr uint32_t RECEIVER_INTERVAL_MS = 5000;
    /// An rfc3550 sender's minimum (Tmin): a chosen 1 s, under RFC 3550 Section 6.2's fixed 5 s.
    static constexpr uint32_t SENDER_INTERVAL_MS = 1000;
    /// The largest compound packet written.
    static constexpr size_t MAX_PACKET = 1024;
    /// SRs whose send times are kept for the round trip: 1.28 s of them at one every 10 ms.
    static constexpr size_t SR_HISTORY = 128;
    /// A sender's SR spacing: one every int(10 ms / ptime) packets (VSF TR-10-1 Section 8.10.1).
    static constexpr uint32_t REPORT_SPACING_US = 10'000;
    /// Datagrams one poll reads at most.
    static constexpr size_t BATCH = 8;

    enum class Role : uint8_t
    {
        sender,
        receiver,
    };

    /// A session for `sender`'s stream (configured): SRs to its RTCP endpoint, marked with its DSCP, on
    /// `options.reports`' schedule; under ipmx `sender` places each run's first packet a lead ahead for its SR
    /// (Sender::hold_reports()). `sender` must outlive the session. Off the real-time thread.
    [[nodiscard]] static auto open_sender(Sender& sender, Transport& transport, RtcpSessionOptions options)
        -> statusbar::StatusValue<std::unique_ptr<RtcpSession>>;
    /// A session for `receiver`'s stream (configured): RRs to its RTCP endpoint, marked with
    /// `options.dscp`. `receiver` must outlive the session.
    [[nodiscard]] static auto open_receiver(Receiver const& receiver, Transport& transport, RtcpSessionOptions options)
        -> statusbar::StatusValue<std::unique_ptr<RtcpSession>>;

    /// Sends a BYE, as close() does.
    ~RtcpSession();
    RtcpSession(RtcpSession const&) = delete;
    auto operator=(RtcpSession const&) -> RtcpSession& = delete;
    RtcpSession(RtcpSession&&) = delete;
    auto operator=(RtcpSession&&) -> RtcpSession& = delete;

    /// Send a report with SDES and BYE (RFC 3550 Sections 6.1, 6.6) and close the port, ending a sender's hold; once.
    void close() noexcept;

    /// The port's descriptor to poll, or -1: tick() then receives.
    [[nodiscard]] auto fd() const noexcept -> int;
    /// Read and parse what has arrived; `now_ns` is the PTP time. A receiver reads only its stream's source's RTCP.
    void on_ready(uint64_t now_ns) noexcept;
    /// Send the report that is due by `now_ns` (PTP), and receive when fd() is -1. A sender's SR goes once the packet
    /// before its own on the grid has left; one whose packet left first is passed over. A sender's SSRC last seen
    /// sending that is gone (stopped, or replaced by a new run's) gets its BYE now. Tick as next_wake_ns() asks.
    void tick(uint64_t now_ns) noexcept;
    /// A sender's clock domain (null, the default: the packet's time is the sender's clock, PTP or a member
    /// interface's): each SR maps its packet's time to PTP time through it and reports the measured rate; none while
    /// the domain is not running. `timebase` must outlive the session.
    void set_timebase(statusbar::itc::SeqlockValue<mediaclock::Timebase> const* const timebase) noexcept { timebase_ = timebase; }
    /// When the session next wants a tick (PTP ns): a receiver's next report, or midway through a sender's next SR
    /// window; nullopt when nothing is due (a sender's next run is found by the next tick).
    [[nodiscard]] auto next_wake_ns(uint64_t now_ns) const noexcept -> std::optional<uint64_t>;
    /// When a receiver's or an rfc3550 sender's next report is due (PTP ns); 0 before the first tick.
    [[nodiscard]] auto next_report_ns() const noexcept -> uint64_t { return next_report_ns_; }

    [[nodiscard]] auto role() const noexcept -> Role { return role_; }
    /// This end's SSRC: the stream's for a sender, a random one for a receiver.
    [[nodiscard]] auto ssrc() const noexcept -> uint32_t;
    [[nodiscard]] auto stats() const noexcept -> RtcpStats const& { return stats_; }
    /// The counts (reports sent and received, BYEs, bad packets, send errors) to zero; what the
    /// remote last reported (loss, jitter, round trip, CNAME, its SR) stays. The ticking thread.
    void reset_stats() noexcept;
    /// RtcpPort::dropped(). Any thread.
    [[nodiscard]] auto dropped() const noexcept -> uint64_t;

  private:
    struct SentReport
    {
        uint32_t lsr{0};
        uint64_t sent_ns{0};
    };

    RtcpSession(Role role, Sender* sender, Receiver const* receiver, std::unique_ptr<RtcpPort> port, RtcpSessionOptions options);
    /// The stream's format: the sender's, else the receiver's.
    [[nodiscard]] static auto format_of(Sender const* sender, Receiver const* receiver) noexcept -> StreamFormat const&;

    /// A packet's first sample index, the time it names (PTP ns), and the measured rate an SR carries.
    struct ReportTime
    {
        uint64_t index{0};
        uint64_t time_ns{0};
        uint32_t measured_rate{0};
    };
    /// A receiver's RR, or an rfc3550 sender's SR, when its cadence says, re-armed after a PTP step back.
    void schedule_report(uint64_t now_ns) noexcept;
    void send_receiver_report(uint64_t now_ns) noexcept;
    /// An rfc3550 sender's SR, naming the next packet to send.
    void send_scheduled_sender_report(uint64_t now_ns) noexcept;
    [[nodiscard]] auto ipmx() const noexcept -> bool { return role_ == Role::sender && options_.reports == SenderReports::ipmx; }
    /// A sender's SR when one is due (due_report()).
    void send_due_report(uint64_t now_ns) noexcept;
    /// The packet whose SR is due for `state`'s run, or nullopt: report_index_ moved past packets that left without
    /// one, and the grid restarted for a new run or a clock that went back.
    [[nodiscard]] auto due_report(SendState const& state) noexcept -> std::optional<uint64_t>;
    /// Frames of `state`'s stream until an ipmx sender next wants a tick (next_wake_ns()).
    [[nodiscard]] auto wake_frames(SendState const& state) const noexcept -> uint64_t;
    /// The SR for the packet `at` names, with SDES; whether it went.
    [[nodiscard]] auto send_sender_report(SendState const& state, ReportTime const& at, uint64_t now_ns) noexcept -> bool;
    /// An empty RR, SDES and BYE for `ssrc` (RFC 3550 Sections 6.1, 6.6); whether it went.
    auto send_bye(uint32_t ssrc) noexcept -> bool;
    /// A sender's BYE for the SSRC it last saw sending, once that is gone.
    void notify_sending() noexcept;
    void write_receiver_report(rtp::RtcpWriter& writer, uint64_t now_ns) noexcept;
    /// The time an SR for the packet at `index` pairs it with (VSF TR-10-1 Section 8.10.1: when that packet's media
    /// clock is sampled); nullopt when the rate is unsupported or the domain is not running.
    [[nodiscard]] auto report_time(uint64_t index) const noexcept -> std::optional<ReportTime>;
    /// report_time() of a sender in a clock domain (timebase_): the packet's domain time `domain_ns` as PTP time.
    [[nodiscard]] auto domain_report_time(uint64_t domain_ns, uint64_t index) const noexcept -> std::optional<ReportTime>;
    /// The rate the Info Block carries for `rate_offset`: kept until it moves a whole hertz.
    [[nodiscard]] auto measured_rate(double rate_offset) const noexcept -> uint32_t;
    /// The SR with the Info Block, remembered for the round trip.
    void write_sender_report(rtp::RtcpWriter& writer, SendState const& state, ReportTime const& at, uint64_t now_ns);
    /// Serialises the Info Block, version `version`, for the sender's current inputs; empty when it does not serialise.
    void rebuild_info_block(uint8_t version, uint32_t measured_sample_rate);
    /// The sender's published state while it is sending since its last configure().
    [[nodiscard]] auto sending_state() const noexcept -> std::optional<SendState>;
    /// The report block on the stream's source (RFC 3550 Section 6.4.1).
    [[nodiscard]] auto report_block(ReceiveState const& state, uint64_t now_ns) noexcept -> rtp::ReportBlock;
    /// The fraction lost since the last report, in 1/256ths (Appendix A.3); restarts on a new source.
    [[nodiscard]] auto fraction_lost(ReceiveState const& state) noexcept -> uint8_t;
    /// The round trip a report block echoing one of the SRs sent gives (RFC 3550 Section 6.4.1).
    [[nodiscard]] auto round_trip_ns(rtp::ReportBlock const& block, uint64_t now_ns) const noexcept -> std::optional<uint64_t>;
    /// The stream's SR: remembered for the next report block, and its time pair recorded.
    void notify_stream_sr(rtp::RtcpSenderReport const& sr, uint64_t now_ns) noexcept;
    /// The remote's CNAME and BYE.
    void notify_remote(rtp::RtcpCompound const& compound, uint32_t remote);
    /// Whether a datagram from `source` is the session's to read: a sender's from anyone, a receiver's only from its
    /// stream's source (ReceiveState::source), since its SRs drive the stream's clock.
    [[nodiscard]] auto from_peer(statusbar::ip::IPv4Address const& source) const noexcept -> bool;
    void handle(std::span<uint8_t const> datagram, uint64_t now_ns) noexcept;
    /// The Info Block for the sender's current inputs (rebuilt when its version or the rate changes).
    void refresh_info_block(uint32_t measured_sample_rate);
    void handle_as_sender(rtp::RtcpCompound const& compound, uint32_t reporter, uint64_t now_ns) noexcept;
    void handle_as_receiver(rtp::RtcpCompound const& compound, uint64_t now_ns) noexcept;
    /// The pair from the stream's SR, when it is in the IPMX form.
    void notify_sender_report(rtp::RtcpSenderReport const& sr, uint64_t now_ns) noexcept;
    void notify_report_block(rtp::ReportBlock const& block, uint64_t now_ns) noexcept;

    Role role_;
    Sender* sender_;
    Sender::ReportHold hold_;  ///< an ipmx sender's, while the session is open
    Receiver const* receiver_;
    std::unique_ptr<RtcpPort> port_;
    RtcpSessionOptions options_;
    statusbar::itc::SeqlockValue<mediaclock::Timebase> const* timebase_{nullptr};
    uint32_t own_ssrc_{0};      ///< a receiver's
    uint32_t sending_ssrc_{0};  ///< a sender's, as its last tick saw it sending (0: not sending)
    bool stopped_bye_{false};   ///< a sender's BYE went for a stop, and it has not sent since
    uint32_t sample_rate_{48000};
    uint64_t minimum_ns_{0};  ///< a receiver's cadence's Tmin, for re-arming after a PTP step
    uint64_t next_report_ns_{0};

    // Sender: the run the SRs follow and the packet the next one names.
    uint64_t report_stride_{0};  ///< frames between SRs: int(10 ms / ptime) packets
    uint32_t report_ssrc_{0};
    uint64_t report_first_{0};
    uint64_t report_index_{0};
    RtcpStats stats_;
    std::array<uint8_t, MAX_PACKET> packet_{};

    // Sender: the Info Block bytes and the SRs sent.
    std::vector<uint8_t> info_block_;
    std::optional<uint8_t> info_block_version_;
    uint32_t info_block_rate_{0};  ///< the measured rate it carries
    uint8_t rate_changes_{0};      ///< added to the sender's version: measured rate changes alone
    std::array<SentReport, SR_HISTORY> sent_{};
    size_t sent_next_{0};

    // Receiver: the remote's last SR and the last report's cumulative figures.
    struct LastSenderReport
    {
        uint32_t lsr{0};
        uint32_t ssrc{0};
        uint64_t arrival_ns{0};
    };
    struct Totals
    {
        uint32_t expected{0};
        uint32_t received{0};
    };
    std::optional<LastSenderReport> last_sr_;
    uint32_t reported_ssrc_{0};
    Totals prior_{};
};

}  // namespace padl::aes67
