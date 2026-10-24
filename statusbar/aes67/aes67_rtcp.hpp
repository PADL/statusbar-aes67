#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An RTCP session beside one AES67 stream (RFC 3550 Section 6): a sender's SR in the IPMX form
/// (PTP truncated time and the IPMX Info Block, VSF TR-10-1 Section 8.7) with SDES CNAME, or a
/// receiver's RR with one report block; BYE on close. Reactor thread only, times in PTP ns.

#include "statusbar/aes67/aes67_receiver.hpp"
#include "statusbar/aes67/aes67_sender.hpp"
#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/rtp/rtcp_interval.hpp"
#include "statusbar/rtp/rtcp_packet.hpp"
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

/// What a session has seen and done. "Remote" is the other end: for a sender, the latest report
/// block about its stream (any receiver's); for a receiver, the stream's sender.
struct RtcpStats
{
    uint64_t sr_sent{0};
    uint64_t rr_sent{0};
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
    /// Send the report that is due by `now_ns` (PTP), and receive when fd() is -1.
    void tick(uint64_t now_ns) noexcept;
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
    /// Write the SR (a sender sending) or RR (a receiver) due; none for a sender not sending.
    auto write_report(rtp::RtcpWriter& writer, uint64_t now_ns) noexcept -> Report;
    void write_receiver_report(rtp::RtcpWriter& writer, uint64_t now_ns) noexcept;
    void handle(std::span<uint8_t const> datagram, uint64_t now_ns) noexcept;
    /// The Info Block for the sender's current inputs (rebuilt when its version changes).
    void refresh_info_block();
    void handle_as_sender(rtp::RtcpCompound const& compound, uint32_t reporter, uint64_t now_ns) noexcept;
    void handle_as_receiver(rtp::RtcpCompound const& compound, uint64_t now_ns) noexcept;
    void take_report_block(rtp::ReportBlock const& block, uint64_t now_ns) noexcept;

    Role role_;
    Sender const* sender_;
    Receiver const* receiver_;
    std::unique_ptr<RtcpPort> port_;
    RtcpSessionOptions options_;
    uint32_t own_ssrc_{0};  ///< a receiver's
    uint32_t sample_rate_{48000};
    uint64_t minimum_ns_{0};  ///< the cadence's Tmin, for re-arming after a PTP step
    uint64_t next_report_ns_{0};
    RtcpStats stats_;
    std::array<uint8_t, MAX_PACKET> packet_{};

    // Sender: the Info Block bytes and the SRs sent.
    std::vector<uint8_t> info_block_;
    std::optional<uint8_t> info_block_version_;
    std::array<SentReport, SR_HISTORY> sent_{};
    size_t sent_next_{0};

    // Receiver: the remote's last SR and the last report's cumulative figures.
    std::optional<uint32_t> last_sr_lsr_;
    uint32_t last_sr_ssrc_{0};
    uint64_t last_sr_arrival_ns_{0};
    uint32_t reported_ssrc_{0};
    uint32_t prior_expected_{0};
    uint32_t prior_received_{0};
};

}  // namespace statusbar::aes67
