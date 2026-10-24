#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// An AES67 sender: frames from a FrameProvider by PTP sample index, sent as
/// RTP, and the stream described in SDP.
///
/// Packets lie on a grid of the format's frames_per_packet: packet k carries
/// indices [k * fpp, (k + 1) * fpp), and is sent on the first tick at which its
/// last frame's index is `read_delay_frames` or more behind the current index
/// (the link offset: a network source delivers frames after their index, an
/// AES67 packet [n, n + 48) no earlier than (n + 48) / fs plus its latency).
/// Its RTP timestamp is
/// PtpMediaClock::rtp_timestamp(first index, 0), so the SDP says
/// `mediaclk:direct=0` and receivers on the same grandmaster place every
/// sample exactly.
///
/// set_packet_phase() shifts the grid by a few frames: senders with different
/// phases send on different ticks, with no added latency and the same timestamps.
///
/// The SSRC is random per configure(), the sequence starts at a
/// random value and counts up, the marker is never set. Frames the provider
/// does not have are sent as silence (zero bytes) and counted lost; with no
/// provider (Supply::none) silence is sent and nothing counted; more than
/// MAX_BURST packets behind, the sender skips ahead. After a backward step of
/// the clock it sends nothing until now catches up with what it has sent
/// (no index goes out twice), unless the step exceeds MAX_BURST packets, when
/// it resynchronises.
///
/// Each packet's payload is rendered in place: tick() hands the provider the
/// payload span of the transport's next TX buffer (SendPort::send_rendered()),
/// after the RTP header, so the frames are copied once, provider to wire
/// buffer. When the transport cannot take the packet (counted dropped) before
/// rendering, the frames are pulled into a staging buffer instead, so a missing
/// pull is counted lost all the same; when it refuses the packet after
/// rendering (the XDP TX ring is full) the frames were already pulled, and the
/// packet is counted dropped without a second pull.
///
/// session() describes the stream for SAP and control: origin the transport's
/// interface address, the group with its TTL, the encoding, channels, rate and
/// ptime, `ts-refclk:ptp=IEEE1588-2008:<EUI-64>:<domain>` and
/// `clock-domain:PTPv2 <domain>` from set_grandmaster() (always PTPv2, the
/// domain always written), `mediaclk:direct=0`, a source filter naming this
/// host for a multicast group, and `a=recvonly` as AES67's examples and Dante
/// write it. Its o= session id is stable across restarts (set_session_id());
/// the version starts at the wall clock's seconds (RFC 4566's suggestion, so
/// it grows across restarts too) and goes up by one with each configure(),
/// grandmaster change and rename (set_name()).
///
/// Threading: configure(), clear(), set_grandmaster(), set_name() and session() allocate:
/// call them off the real-time thread. tick() and the accessors are real-time
/// safe.

#include "statusbar/aes67/aes67_stream.hpp"
#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/sdp/sdp.hpp"
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

namespace statusbar::aes67 {

/// A grandmaster written `<EUI-64>[:<domain>]` (the EUI-64 with '-' or ':'
/// separators, or none; the domain 0-127), as a command line or configuration
/// gives it; nullopt if it is not one.
[[nodiscard]] auto parse_grandmaster(std::string_view text) -> std::optional<Grandmaster>;

class Sender
{
  public:
    /// Packets one tick sends at most; further behind, the sender skips ahead.
    static constexpr uint32_t MAX_BURST = 8;
    /// The largest payload sent: a 1500-byte MTU less IPv4, UDP and RTP headers.
    static constexpr uint32_t MAX_PAYLOAD = 1500 - 20 - 8 - 12;
    /// Session ids and versions are kept to 63 bits: positive in any signed reading.
    static constexpr uint64_t SESSION_ID_MASK = 0x7FFF'FFFF'FFFF'FFFFULL;

    /// `name` is the session name (s=) of session().
    Sender(StreamFormat const& format, FrameProvider provider, Transport& transport, std::string name);

    /// Send to `destination`, replacing any before.
    [[nodiscard]] auto configure(Destination const& destination) -> Status;
    /// Send to `session.media[0]`'s connection, port and payload type; its
    /// encoding, channels and rate must be the sender's.
    [[nodiscard]] auto configure(sdp::Session const& session) -> Status;
    /// Stop sending.
    void clear() noexcept;

    /// The read delay (link offset) in frames: a packet is sent once its last
    /// frame is this far behind now. Default: default_read_delay(). Any thread,
    /// also while the sender is ticked (a change takes effect at the next tick:
    /// a longer delay waits, a shorter one catches up, up to MAX_BURST
    /// packets); real-time safe.
    void set_read_delay(uint32_t const frames) noexcept { read_delay_.store(frames, std::memory_order_relaxed); }
    [[nodiscard]] auto read_delay() const noexcept -> uint32_t { return read_delay_.load(std::memory_order_relaxed); }
    /// One 125 us tick (rounded up: 6 frames at 48 kHz): a packet goes out as
    /// soon as its last frame is available, for a provider whose frames are
    /// there by their index (generated or captured locally, or placed ahead of
    /// time), so a receiver sees it about a packet time after its RTP
    /// timestamp (1.125 ms for 1 ms packets at 48 kHz). The trade-off: the
    /// lower the delay, the more of a receiver's latency budget is left (a
    /// Dante receiver at its 1 ms setting is already late with 1 ms packets),
    /// but a provider whose frames land later than that (relayed from the
    /// network) has them counted lost: use relay_read_delay() for one.
    [[nodiscard]] static constexpr auto default_read_delay(StreamFormat const& format) noexcept -> uint32_t
    {
        return (format.sample_rate + 7999) / 8000;
    }
    /// One packet of this stream plus two 125 us ticks: for a provider whose
    /// frames arrive over a network (an AES67 receiver's), a packet time plus
    /// arrival jitter after their index.
    [[nodiscard]] static constexpr auto relay_read_delay(StreamFormat const& format) noexcept -> uint32_t
    {
        return format.frames_per_packet + (format.sample_rate / 4000);
    }

    /// Where the packet grid starts, in frames (taken modulo frames_per_packet):
    /// packets carry [k * fpp + phase, (k + 1) * fpp + phase). Call before
    /// configure(), off the real-time thread.
    void set_packet_phase(uint32_t const frames) noexcept
    {
        phase_ = format_.frames_per_packet != 0 ? frames % format_.frames_per_packet : 0;
    }
    [[nodiscard]] auto packet_phase() const noexcept -> uint32_t { return phase_; }
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

    /// The SDP session id (o=), stable for the same stream across restarts
    /// (RFC 4566: a receiver, Dante Controller say, keys a session on it and
    /// the origin, so a new id each start leaves stale sessions behind). 0
    /// (the default): the first configure() derives one, stable_session_id()
    /// of the origin address and the name (so two senders on one host with
    /// the same name need ids of their own). Call before configure(), off the
    /// real-time thread.
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

    /// The PTP grandmaster ts-refclk names; nullopt until it is known, and the
    /// SDP then has no ts-refclk. A change bumps the session version (o=), as
    /// configure() does.
    void set_grandmaster(std::optional<Grandmaster> const& grandmaster);

    [[nodiscard]] auto grandmaster() const noexcept -> std::optional<Grandmaster> const& { return grandmaster_; }

    /// The session name (s=) of session(), the constructor's until changed:
    /// a new one bumps the session version, as set_grandmaster() does (a
    /// receiver keying on the name sees a new session). Off the real-time
    /// thread.
    void set_name(std::string name);
    [[nodiscard]] auto name() const noexcept -> std::string const& { return name_; }

    /// Take what `previous` was given outside its destination: the read delay,
    /// the grandmaster, the session name, and the SDP session id and version (so a replacement
    /// sender's next configure() announces the same session, one version on).
    /// Call before configure(), off the real-time thread.
    void continue_from(Sender const& previous);

    /// Send every packet due by `now_index` (the media clock's index at the
    /// current PTP time).
    void tick(uint64_t now_index) noexcept;

    /// The stream's SDP, or nullopt when not configured.
    [[nodiscard]] auto session() const -> std::optional<sdp::Session>;

    [[nodiscard]] auto format() const noexcept -> StreamFormat const& { return format_; }
    [[nodiscard]] auto state() const noexcept -> State { return state_; }
    [[nodiscard]] auto counters() const noexcept -> Counters const& { return counters_; }
    /// Why the transport refused the last configure(), or why it cannot send
    /// (Transport::send_error(), seen by the last tick), for State::error.
    [[nodiscard]] auto error() const noexcept -> std::error_code { return error_; }
    [[nodiscard]] auto configured() const noexcept -> bool { return port_ != nullptr; }
    [[nodiscard]] auto ssrc() const noexcept -> uint32_t { return ssrc_; }

  private:
    StreamFormat format_;
    FrameProvider provider_;
    Transport& transport_;
    std::string name_;
    /// Fill `payload` with the provider's frames from `first_index`, or
    /// silence when it has none; what the provider said.
    /// One version on (the first: the wall clock's seconds, one on).
    void bump_session_version();
    [[nodiscard]] auto render_payload(uint64_t first_index, std::span<uint8_t> payload) noexcept -> Supply;

    std::unique_ptr<SendPort> port_;
    Destination destination_;
    std::vector<uint8_t> payload_;  ///< staging, only for a packet the transport had no TX buffer for

    uint32_t ssrc_{0};
    uint16_t sequence_{0};
    uint64_t session_id_{0};
    uint64_t session_version_{0};
    std::optional<Grandmaster> grandmaster_;
    std::atomic<uint32_t> read_delay_;
    uint32_t phase_{0};
    bool started_{false};
    uint64_t next_index_{0};  ///< first frame of the next packet to send
    Counters counters_;
    State state_{State::idle};
    std::error_code error_;
};

}  // namespace statusbar::aes67
