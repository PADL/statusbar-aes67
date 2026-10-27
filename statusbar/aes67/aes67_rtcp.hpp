#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An RTCP session beside one AES67 stream (RFC 3550 Section 6): a sender's SR in the IPMX form
/// (PTP truncated time and the IPMX Info Block, VSF TR-10-1 Section 8.7) with SDES CNAME, or a
/// receiver's RR with one report block; BYE on close. Reactor thread only, times in PTP ns.

#include "statusbar/aes67/aes67_receiver.hpp"
#include "statusbar/aes67/aes67_sender.hpp"
#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/mediaclock/mediaclock_timebase.hpp"
#include "statusbar/rtp/rtcp_interval.hpp"
#include "statusbar/rtp/rtcp_packet.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace statusbar::aes67 {

/// When a session's next report goes: the cadence strategy.
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

/// A sender report's pair (RFC 3550 Section 6.4.1): an RTP timestamp and the PTP time it names, from
/// an SR in the IPMX form (TR-10-1 Section 8.7), and when the SR arrived (PTP ns).
struct SenderReportTime
{
    uint32_t rtp_timestamp{0};
    uint64_t ptp_ns{0};
    uint64_t arrival_ns{0};

    auto operator==(SenderReportTime const&) const noexcept -> bool = default;
};

/// Called on the reactor with each of the stream's sender reports that carries the pair.
using SenderReportHandler = sg14::inplace_function<void(SenderReportTime const&), 32>;

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

/// What a session is opened with.
struct RtcpSessionOptions
{
    std::string cname;           ///< RFC 3550 Section 6.5.1: "<identity>@<address>", at most 255 bytes
    uint32_t interval_ms{1000};  ///< a sender's SR interval (Tmin); a receiver's RR interval is RECEIVER_INTERVAL_MS
    /// A receiver's RTCP DSCP: what it would send a similar stream with (AES67-2018 Section 6.2).
    /// A sender's is its stream's.
    uint8_t dscp{rtp::FrameAddressing::DSCP_MEDIA};
    std::unique_ptr<RtcpCadence> cadence;  ///< null: Rfc3550Interval with a random seed
    SenderReportHandler on_sender_report;  ///< receiver: each new RtcpStats::remote_sr; may be empty
};

class RtcpSession
{
  public:
    /// RFC 3550 Section 6.2's fixed minimum, which a receiver (not an active sender) keeps.
    static constexpr uint32_t RECEIVER_INTERVAL_MS = 5000;
    /// The largest compound packet written.
    static constexpr size_t MAX_PACKET = 1024;
    /// SRs whose send times are kept for the round trip.
    static constexpr size_t SR_HISTORY = 8;
    /// Datagrams one poll takes at most.
    static constexpr size_t BATCH = 8;

    enum class Role : uint8_t
    {
        sender,
        receiver,
    };

    /// A session for `sender`'s stream (configured): SRs to its RTCP endpoint, marked with its DSCP.
    /// `sender` must outlive the session; `options.interval_ms` must not be 0 (invalid_argument).
    [[nodiscard]] static auto open_sender(Sender const& sender, Transport& transport, RtcpSessionOptions options)
        -> StatusValue<std::unique_ptr<RtcpSession>>;
    /// A session for `receiver`'s stream (configured): RRs to its RTCP endpoint, marked with
    /// `options.dscp`. `receiver` must outlive the session.
    [[nodiscard]] static auto open_receiver(Receiver const& receiver, Transport& transport, RtcpSessionOptions options)
        -> StatusValue<std::unique_ptr<RtcpSession>>;

    /// Sends a BYE, as close() does.
    ~RtcpSession();
    RtcpSession(RtcpSession const&) = delete;
    auto operator=(RtcpSession const&) -> RtcpSession& = delete;
    RtcpSession(RtcpSession&&) = delete;
    auto operator=(RtcpSession&&) -> RtcpSession& = delete;

    /// Send a report with SDES and BYE (RFC 3550 Sections 6.1, 6.6) and close the port; once.
    void close() noexcept;

    /// The port's descriptor to poll, or -1: tick() then receives.
    [[nodiscard]] auto fd() const noexcept -> int;
    /// Take and parse what has arrived; `now_ns` is the PTP time.
    void on_ready(uint64_t now_ns) noexcept;
    /// Send the report that is due by `now_ns` (PTP), and receive when fd() is -1. A sender's SSRC
    /// last seen sending that is gone (stopped, or replaced by a new run's) gets its BYE now.
    void tick(uint64_t now_ns) noexcept;
    /// A sender's clock domain (null, the default: PTP time): each SR reads it, pairs the domain's
    /// index with its PTP time through it, and reports the measured rate; none while not running.
    /// `timebase` must outlive the session.
    void set_timebase(itc::SeqlockValue<mediaclock::Timebase> const* const timebase) noexcept { timebase_ = timebase; }
    /// When the next report is due (PTP ns); 0 before the first tick.
    [[nodiscard]] auto next_report_ns() const noexcept -> uint64_t { return next_report_ns_; }

    [[nodiscard]] auto role() const noexcept -> Role { return role_; }
    /// This end's SSRC: the stream's for a sender, a random one for a receiver.
    [[nodiscard]] auto ssrc() const noexcept -> uint32_t;
    [[nodiscard]] auto stats() const noexcept -> RtcpStats const& { return stats_; }
    /// RtcpPort::dropped(). Any thread.
    [[nodiscard]] auto dropped() const noexcept -> uint64_t;

  private:
    struct SentReport
    {
        uint32_t lsr{0};
        uint64_t sent_ns{0};
    };

    RtcpSession(
        Role role, Sender const* sender, Receiver const* receiver, std::unique_ptr<RtcpPort> port, RtcpSessionOptions options);

    enum class Report : uint8_t
    {
        none,
        sr,
        rr,
    };

    void send_report(uint64_t now_ns) noexcept;
    /// An empty RR, SDES and BYE for `ssrc` (RFC 3550 Sections 6.1, 6.6); whether it went.
    auto send_bye(uint32_t ssrc) noexcept -> bool;
    /// A sender's BYE for the SSRC it last saw sending, once that is gone.
    void note_sending() noexcept;
    /// Write the SR (a sender sending) or RR (a receiver) due; none for a sender not sending.
    auto write_report(rtp::RtcpWriter& writer, uint64_t now_ns) noexcept -> Report;
    void write_receiver_report(rtp::RtcpWriter& writer, uint64_t now_ns) noexcept;
    /// A sample index, its presentation time in PTP ns, and the measured rate an SR carries.
    struct ReportTime
    {
        uint64_t index{0};
        uint64_t time_ns{0};
        uint32_t measured_rate{0};
    };
    /// The index and time an SR at `now_ns` pairs; nullopt when the rate or the domain's outputs are not sending.
    [[nodiscard]] auto report_time(uint64_t now_ns) const noexcept -> std::optional<ReportTime>;
    /// report_time() in the sender's media clock domain (timebase_).
    [[nodiscard]] auto domain_report_time(mediaclock::PtpMediaClock const& clock, uint64_t now_ns) const noexcept
        -> std::optional<ReportTime>;
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
    [[nodiscard]] auto take_fraction_lost(ReceiveState const& state) noexcept -> uint8_t;
    /// The round trip a report block echoing one of the SRs sent gives (RFC 3550 Section 6.4.1).
    [[nodiscard]] auto round_trip_ns(rtp::ReportBlock const& block, uint64_t now_ns) const noexcept -> std::optional<uint64_t>;
    /// The stream's SR: remembered for the next report block, and its time pair taken.
    void take_stream_sr(rtp::RtcpSenderReport const& sr, uint64_t now_ns) noexcept;
    /// The remote's CNAME and BYE.
    void note_remote(rtp::RtcpCompound const& compound, uint32_t remote);
    void handle(std::span<uint8_t const> datagram, uint64_t now_ns) noexcept;
    /// The Info Block for the sender's current inputs (rebuilt when its version or the rate changes).
    void refresh_info_block(uint32_t measured_sample_rate);
    void handle_as_sender(rtp::RtcpCompound const& compound, uint32_t reporter, uint64_t now_ns) noexcept;
    void handle_as_receiver(rtp::RtcpCompound const& compound, uint64_t now_ns) noexcept;
    /// The pair from the stream's SR, when it is in the IPMX form.
    void take_sender_report(rtp::RtcpSenderReport const& sr, uint64_t now_ns) noexcept;
    void take_report_block(rtp::ReportBlock const& block, uint64_t now_ns) noexcept;

    Role role_;
    Sender const* sender_;
    Receiver const* receiver_;
    std::unique_ptr<RtcpPort> port_;
    RtcpSessionOptions options_;
    itc::SeqlockValue<mediaclock::Timebase> const* timebase_{nullptr};
    uint32_t own_ssrc_{0};      ///< a receiver's
    uint32_t sending_ssrc_{0};  ///< a sender's, as its last tick saw it sending (0: not sending)
    bool stopped_bye_{false};   ///< a sender's BYE went for a stop, and it has not sent since
    uint32_t sample_rate_{48000};
    uint64_t minimum_ns_{0};  ///< the cadence's Tmin, for re-arming after a PTP step
    uint64_t next_report_ns_{0};
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

}  // namespace statusbar::aes67
