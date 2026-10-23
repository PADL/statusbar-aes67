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
/// sample exactly. The SSRC is random per configure(), the sequence starts at a
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
/// buffer. Only when the transport cannot take the packet (counted dropped)
/// are the frames pulled into a staging buffer instead, so a missing pull is
/// counted lost all the same.
///
/// session() describes the stream for SAP and control: origin the transport's
/// interface address, the group with its TTL, the encoding, channels, rate and
/// ptime, `ts-refclk:ptp=IEEE1588-2008:<EUI-64>:<domain>` and
/// `clock-domain:PTPv2 <domain>` from set_grandmaster() (always PTPv2, the
/// domain always written), `mediaclk:direct=0`, a source filter naming this
/// host for a multicast group, and `a=recvonly` as AES67's examples and Dante
/// write it.
///
/// Threading: configure(), clear(), set_grandmaster() and session() allocate:
/// call them off the real-time thread. tick() and the accessors are real-time
/// safe.

#include "statusbar/aes67/aes67_stream.hpp"
#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/sdp/sdp.hpp"
#include "statusbar/status/status.hpp"

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
    /// frame is this far behind now. Default: default_read_delay(). Any thread
    /// before the sender is ticked; real-time safe.
    void set_read_delay(uint32_t const frames) noexcept { read_delay_ = frames; }
    [[nodiscard]] auto read_delay() const noexcept -> uint32_t { return read_delay_; }
    /// One packet of this stream plus two 125 us ticks.
    [[nodiscard]] static constexpr auto default_read_delay(StreamFormat const& format) noexcept -> uint32_t
    {
        return format.frames_per_packet + (format.sample_rate / 4000);
    }

    /// The PTP grandmaster ts-refclk names; nullopt until it is known, and the
    /// SDP then has no ts-refclk. A change bumps the session version (o=), as
    /// configure() does.
    void set_grandmaster(std::optional<Grandmaster> const& grandmaster);

    [[nodiscard]] auto grandmaster() const noexcept -> std::optional<Grandmaster> const& { return grandmaster_; }

    /// Take what `previous` was given outside its destination: the read delay,
    /// the grandmaster, and the SDP session id and version (so a replacement
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
    /// Why the transport refused the last configure(), for State::error.
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
    [[nodiscard]] auto render_payload(uint64_t first_index, std::span<uint8_t> payload) noexcept -> Supply;

    std::unique_ptr<SendPort> port_;
    Destination destination_;
    std::vector<uint8_t> payload_;  ///< staging, only for a packet the transport had no TX buffer for

    uint32_t ssrc_{0};
    uint16_t sequence_{0};
    uint64_t session_id_{0};
    uint64_t session_version_{0};
    std::optional<Grandmaster> grandmaster_;
    uint32_t read_delay_;
    bool started_{false};
    uint64_t next_index_{0};  ///< first frame of the next packet to send
    Counters counters_;
    State state_{State::idle};
    std::error_code error_;
};

}  // namespace statusbar::aes67
