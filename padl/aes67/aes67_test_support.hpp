#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// Test support: an in-memory Transport, a pattern source and a recording sink, and crafted
/// sessions and RTP packets, for the tests of AES67 senders and receivers.

#include "padl/aes67/aes67_receiver.hpp"
#include "padl/aes67/aes67_sender.hpp"
#include "padl/aes67/aes67_transport.hpp"
#include "padl/rtp/rtp_header.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace padl::aes67 {

/// Frames per packet: a 1 ms ptime at 48 kHz.
inline constexpr uint32_t FPP = 48;

/// A loopback address of this process's own, 127.(1-64).x.y from the pid: the suite runs in
/// several processes at once (ctest -j, one per file and one for all), and receivers bind their
/// destination address, so no other process's datagrams reach these tests' ports.
inline auto own_loopback() -> statusbar::ip::IPv4Address
{
    auto const pid = static_cast<uint32_t>(::getpid());
    return statusbar::ip::IPv4Address{
        127, static_cast<uint8_t>(1 + ((pid >> 16U) % 64)), static_cast<uint8_t>(pid >> 8U), static_cast<uint8_t>(pid)};
}

/// A multicast group of this process's own (239.(64-127).x.y from the pid), for the same reason.
inline auto own_group() -> statusbar::ip::IPv4Address
{
    auto const pid = static_cast<uint32_t>(::getpid());
    return statusbar::ip::IPv4Address{
        239, static_cast<uint8_t>(64 + ((pid >> 16U) % 64)), static_cast<uint8_t>(pid >> 8U), static_cast<uint8_t>(pid)};
}

/// A PTP grandmaster in domain 0, for senders to name.
inline Grandmaster const GRANDMASTER{.identity = *statusbar::ieee::eui64_from_string("00-1D-C1-FF-FE-12-34-56"), .domain = 0};

/// Two channels of L24 at 48 kHz, FPP frames per packet.
inline constexpr StreamFormat STEREO_L24{
    .encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP};
/// Two channels of L32 at 48 kHz, FPP frames per packet.
inline constexpr StreamFormat STEREO_L32{
    .encoding = sdp::Encoding::l32, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP};

/// Byte `b` of frame `index`: a pattern a receiver can check by index.
inline auto pattern(uint64_t const index, size_t const b) -> uint8_t
{
    return static_cast<uint8_t>((index * 7) + b);
}

/// Generates the pattern for any index, or refuses indices in `missing`.
class PatternSource
{
  public:
    /// A source of `frame_bytes` octets a frame.
    explicit PatternSource(uint32_t const frame_bytes)
        : frame_bytes_{frame_bytes}
    {}

    /// A FrameProvider that pull()s from this source, which must outlive it.
    auto provider() -> FrameProvider
    {
        return [this](uint64_t const first_index, uint32_t const frames, std::span<uint8_t> const out) {
            return pull(first_index, frames, out) ? Supply::frames : Supply::missing;
        };
    }

    /// `frames` frames of the pattern from `first_index` into `out`; false for refuse_index.
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

    uint64_t refuse_index{UINT64_MAX};  ///< the first index pull() refuses
    size_t pulls{0};                    ///< pull() calls

  private:
    uint32_t frame_bytes_;
};

/// A block a receiver handed on: its first index, its frame count and a copy of its bytes.
struct Received
{
    uint64_t first_index;        ///< the index of its first frame
    uint32_t frames;             ///< its frame count
    std::vector<uint8_t> bytes;  ///< its payload
};

/// Records every block a receiver hands on.
class RecordingSink
{
  public:
    /// A FrameHandler appending to blocks; this sink must outlive it.
    auto handler() -> FrameHandler
    {
        return [this](uint64_t const first_index, uint32_t const frames, std::span<uint8_t const> const bytes) {
            blocks.push_back({first_index, frames, {bytes.begin(), bytes.end()}});
        };
    }
    std::vector<Received> blocks;  ///< every block, in order
};

/// A transport in memory: what senders send, receivers receive.
class LoopTransport final : public Transport
{
  public:
    /// A packet a send port sent, for the receive ports of its destination.
    struct Packet
    {
        Endpoint destination;                            ///< where it was sent
        std::vector<uint8_t> bytes;                      ///< the whole RTP packet
        std::optional<statusbar::ieee::VlanTag> vlan{};  ///< the port's tag when it was sent
    };

    /// A receive port: the queued packets for its destination, from `source`.
    class Rx final : public ReceivePort
    {
      public:
        /// A port on `t` for `d`; `t` must outlive it.
        Rx(LoopTransport& t, Endpoint const& d)
            : transport{t}
            , destination{d}
        {}
        /// The queued packets for its destination, others dropped; each held until the next call.
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
        /// The transport's port_error.
        [[nodiscard]] auto error() const noexcept -> std::error_code override { return transport.port_error; }
        LoopTransport& transport;               ///< where its packets come from
        Endpoint destination;                   ///< what it receives
        std::deque<std::vector<uint8_t>> held;  ///< the datagrams the last receive() returned
    };

    /// A send port: every packet queued for its destination.
    class Tx final : public SendPort
    {
      public:
        /// A port on `t` to `d`; `t` must outlive it.
        Tx(LoopTransport& t, Endpoint const& d)
            : transport{t}
            , destination{d}
        {}
        /// The packet queued, or false (render not called) when the transport refuses.
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
            transport.queue.push_back({destination, std::move(bytes), transport.vlan});
            return true;
        }
        /// send_rendered(), its departure recorded in departures.
        auto send_rendered_at(
            rtp::RtpHeader const& header,
            size_t const payload_length,
            PayloadRenderer const& render,
            uint64_t const departure_ns) noexcept -> bool override
        {
            transport.departures.push_back(departure_ns);
            return send_rendered(header, payload_length, render);
        }
        /// Nothing batched: 0.
        auto flush() noexcept -> size_t override { return 0; }
        /// The tag recorded in vlan; refused, as a socket refuses it, under refuse_tag.
        auto set_vlan(std::optional<statusbar::ieee::VlanTag> const& vlan) noexcept -> statusbar::Status override
        {
            if (transport.refuse_tag && vlan) {
                return SendPort::set_vlan(vlan);
            }
            transport.vlan = vlan;
            return statusbar::success();
        }
        LoopTransport& transport;  ///< where its packets go
        Endpoint destination;      ///< where it sends
    };

    /// The source address its packets arrive from.
    [[nodiscard]] auto local_address() const noexcept -> statusbar::ip::IPv4Address override { return source.address; }
    /// An Rx for `destination`, joined for any source.
    auto open_receive(Endpoint const& destination) -> statusbar::StatusValue<std::unique_ptr<ReceivePort>> override
    {
        joined_source.reset();
        return std::make_unique<Rx>(*this, destination);
    }
    /// An Rx for `destination`, joined for `from` alone (joined_source).
    auto open_receive_from(Endpoint const& destination, statusbar::ip::IPv4Address const& from)
        -> statusbar::StatusValue<std::unique_ptr<ReceivePort>> override
    {
        joined_source = from;
        return std::make_unique<Rx>(*this, destination);
    }
    /// A Tx to the options' destination, with their tag.
    auto open_send(SendOptions const& options) -> statusbar::StatusValue<std::unique_ptr<SendPort>> override
    {
        vlan = options.vlan;
        return std::make_unique<Tx>(*this, options.destination);
    }

    /// failing.
    [[nodiscard]] auto send_error() const noexcept -> std::error_code override { return failing; }
    /// every_tick.
    [[nodiscard]] auto drain_every_tick() const noexcept -> bool override { return every_tick; }

    std::deque<Packet> queue;          ///< sent and not yet received
    std::vector<uint64_t> departures;  ///< each packet's departure as the sender gave it
    /// The address and port every packet arrives from.
    Endpoint source{.address = statusbar::ip::IPv4Address{192, 168, 1, 10}, .port = 5004};
    bool refuse{false};                                       ///< every send_rendered() finds no TX buffer
    std::error_code failing;                                  ///< send_error()
    std::error_code port_error;                               ///< every receive port's error()
    bool every_tick{false};                                   ///< drain_every_tick()
    bool refuse_tag{false};                                   ///< set_vlan() refuses a tag, as a socket does
    std::optional<statusbar::ieee::VlanTag> vlan;             ///< the open port's tag
    size_t receives{0};                                       ///< receive() calls
    std::optional<statusbar::ip::IPv4Address> joined_source;  ///< the last port's source-specific join, if it was one
};

/// A two-channel L24 stream to `destination`, PT 97, on a direct media clock at `offset` (none: not PTP-locked).
inline auto stream_session(Endpoint const& destination, std::optional<uint32_t> const offset = 0) -> sdp::Session
{  // NOLINT(statusbar-block-statements): a crafted session, field by field
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

/// FPP frames of STEREO_L24's pattern from index `timestamp`.
inline auto pattern_payload(uint32_t const timestamp) -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload(size_t{FPP} * 6);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = pattern(timestamp + (i / 6), i % 6);
    }
    return payload;
}

/// An RTP packet of pattern_payload(`timestamp`).
inline auto rtp_packet(uint16_t const seq, uint32_t const timestamp, uint32_t const ssrc = 0x1234, uint8_t const pt = 97)
    -> std::vector<uint8_t>
{
    auto const payload = pattern_payload(timestamp);
    rtp::RtpHeader header{};
    header.init(pt, seq, timestamp, ssrc);
    std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + payload.size());
    (void)rtp::serialize_rtp(header, payload, bytes);
    return bytes;
}

/// Whether every big-endian word of `bytes` is `word`.
inline auto all_words_are(std::span<uint8_t const> const bytes, uint32_t const word) -> bool
{
    if (bytes.size() % 4 != 0) {
        return false;
    }
    for (size_t at = 0; at < bytes.size(); at += 4) {
        statusbar::ieee::quadlet_t got{};
        statusbar::span_load(got, bytes.subspan(at, 4));
        if (got.get() != word) {
            return false;
        }
    }
    return true;
}

}  // namespace padl::aes67
