#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// What an AES67 receiver or sender is: its stream format, its FrameHandler or FrameProvider (by
/// PTP sample index, on the real-time thread, never allocating), its destination, counters, state,
/// and the figures it publishes for its RTCP session.

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/sdp/sdp.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace statusbar::aes67 {

/// A stream as AES67 carries it: the rtpmap's encoding, channels and rate,
/// and the ptime as frames per packet. A frame is one sample of every
/// channel; on the wire it is the channels' samples in order.
struct StreamFormat
{
    sdp::Encoding encoding{sdp::Encoding::l24};
    uint16_t channels{1};
    uint32_t sample_rate{48000};
    uint32_t frames_per_packet{48};  ///< the ptime: 48 at 1 ms, 6 at 125 us (48 kHz)

    [[nodiscard]] constexpr auto bytes_per_frame() const noexcept -> uint32_t { return channels * sdp::bytes_per_sample(encoding); }
    [[nodiscard]] constexpr auto bytes_per_packet() const noexcept -> uint32_t { return bytes_per_frame() * frames_per_packet; }
    /// The ptime in microseconds, as SDP writes it.
    [[nodiscard]] constexpr auto packet_time_us() const noexcept -> uint32_t
    {
        return sample_rate == 0 ? 0 : static_cast<uint32_t>((uint64_t{frames_per_packet} * 1'000'000) / sample_rate);
    }

    auto operator==(StreamFormat const&) const noexcept -> bool = default;
};

/// One silent sample of `encoding` as a big-endian word: zero for linear PCM; for AM824 the
/// MBLA label 0x40 with a zero sample (IEC 61883-6 Clause 6.2), as a zero word is IEC 60958 data.
[[nodiscard]] constexpr auto silence_word(sdp::Encoding const encoding) noexcept -> uint32_t
{
    return encoding == sdp::Encoding::am824 ? 0x4000'0000U : 0U;
}

/// Fill `out` (whole samples of `encoding`) with silence: zero bytes, or the AM824 word repeated.
inline auto fill_silence(sdp::Encoding const encoding, std::span<uint8_t> const out) noexcept -> void
{
    uint32_t const word = silence_word(encoding);
    if (word == 0 || out.size() < sizeof(word)) {
        span_zero(out);
        return;
    }
    span_store(out, ieee::quadlet_t{word});
    for (size_t filled = sizeof(word); filled < out.size(); filled *= 2) {
        span_copy(out.subspan(filled), out.first(filled));
    }
}

/// A receiver's frames: `frames` frames from sample index `first_index`, the
/// stream's wire bytes. `bytes` is valid only during the call.
using FrameHandler = sg14::inplace_function<void(uint64_t first_index, uint32_t frames, std::span<uint8_t const> bytes), 64>;

/// What a FrameProvider gave a sender.
enum class Supply : uint8_t
{
    frames,   ///< `out` holds the frames
    missing,  ///< the source does not have them: the sender sends silence and counts it lost
    none,     ///< nothing feeds the sender: it sends silence, and nothing is lost
};

/// A sender's frames: fill `out` (exactly `frames` frames of the stream's
/// format) from sample index `first_index`.
using FrameProvider = sg14::inplace_function<Supply(uint64_t first_index, uint32_t frames, std::span<uint8_t> out), 64>;

/// Where a sender sends.
struct Destination
{
    ip::IPv4Address address;  ///< multicast or unicast
    uint16_t port{5004};
    uint8_t payload_type{96};
    uint8_t ttl{sdp::Connection::DEFAULT_MULTICAST_TTL};  ///< for a multicast address

    auto operator==(Destination const&) const noexcept -> bool = default;
};

/// The PTP grandmaster a sender's ts-refclk names. AES67 references are
/// always PTPv2 (IEEE 1588-2008); an 802.1AS grandmaster has the same EUI-64.
struct Grandmaster
{
    ieee::Eui64 identity{};
    uint8_t domain{0};  ///< 0-127

    auto operator==(Grandmaster const&) const noexcept -> bool = default;
};

enum class State : uint8_t
{
    idle,     ///< not configured
    ready,    ///< configured, nothing received or sent yet
    running,  ///< packets flowing
    error,    ///< the transport refused the last configure(), or (a sender) refuses to send: Transport::send_error()
};

struct Counters
{
    uint64_t packets{0};  ///< packets handed on (receiver) or sent (sender)
    uint64_t frames{0};
    uint64_t lost{0};     ///< receiver: frames in sequence gaps not yet filled; sender: frames the source lacked, or skipped
    uint64_t late{0};     ///< receiver: frames that arrived out of order (still handed on)
    uint64_t dropped{0};  ///< packets refused (receiver) or not sent (sender)

    auto operator==(Counters const&) const noexcept -> bool = default;
};

/// What a sender publishes for RTCP at the end of each tick (RFC 3550 Section 6.4.1 sender info).
struct SendState
{
    uint32_t ssrc{0};     ///< 0 before the first tick after configure()
    uint32_t packets{0};  ///< sent since configure(), mod 2^32
    uint32_t octets{0};   ///< payload octets sent since configure(), mod 2^32

    auto operator==(SendState const&) const noexcept -> bool = default;
};

/// What a receiver publishes for RTCP after each drain that accepted a packet (RFC 3550 Section
/// 6.4.1 report block inputs, cumulative; the session takes interval differences).
struct ReceiveState
{
    uint32_t ssrc{0};
    bool valid{false};           ///< a source has been heard
    uint32_t extended_max{0};    ///< rtp::RtpSequence::extended_max()
    uint32_t expected{0};        ///< rtp::RtpSequence::expected()
    uint32_t received{0};        ///< rtp::RtpSequence::received()
    uint32_t jitter_x16{0};      ///< interarrival jitter, timestamp units x 16 (Appendix A.8)
    uint32_t last_rtp_ts{0};     ///< the newest packet's RTP timestamp
    uint64_t last_now_index{0};  ///< the drain's index when it arrived

    auto operator==(ReceiveState const&) const noexcept -> bool = default;
};

}  // namespace statusbar::aes67
