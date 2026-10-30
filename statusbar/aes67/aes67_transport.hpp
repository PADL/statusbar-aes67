#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// How AES67 receivers and senders reach the network: a Transport opens a ReceivePort per receiver,
/// a SendPort per sender and an RtcpPort per RTCP session. The control thread opens and destroys
/// ports, safely while the real-time thread ticks; it brackets each tick with begin_tick() / end_tick().

#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/rtp/rtp_frame.hpp"
#include "statusbar/rtp/rtp_header.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

namespace statusbar::aes67 {

/// An IPv4 address and UDP port.
struct Endpoint
{
    ip::IPv4Address address;
    uint16_t port{0};

    auto operator==(Endpoint const&) const noexcept -> bool = default;
};

/// The RTCP endpoint beside a stream's: the same address, the next port (RFC 3550 Section 11).
[[nodiscard]] constexpr auto rtcp_endpoint_of(Endpoint const& media) noexcept -> Endpoint
{
    return {.address = media.address, .port = static_cast<uint16_t>(media.port + 1)};
}

/// A received UDP payload. `payload` is valid until the next receive() on the
/// same port or the next begin_tick(), whichever is first.
struct Datagram
{
    std::span<uint8_t const> payload;
    Endpoint source;
};

/// True when `datagram` is RTCP, as on a port carrying RTP and RTCP alike: version 2 and a packet type
/// of 192 to 223, which RTP payload types never take there (RFC 5761 Section 4).
[[nodiscard]] constexpr auto is_multiplexed_rtcp(std::span<uint8_t const> const datagram) noexcept -> bool
{
    return datagram.size() >= 2 && (datagram[0] >> 6U) == 2 && datagram[1] >= 192 && datagram[1] <= 223;
}

/// Why a transport refused a received frame before any receiver saw it.
enum class Refusal : uint8_t
{
    malformed,  ///< not a whole IPv4/UDP frame (cut short, or lengths past its end), or truncated
    unmatched,  ///< no receive port or RTCP port for its destination address and port (AF_XDP)
    not_rtp,    ///< for a receive port's destination, but not an RTP packet
    port_full,  ///< a receive port already held as many datagrams as it keeps for one tick
};

/// A transport's refused frames by Refusal, since it opened.
struct ReceiveRefusals
{
    uint64_t malformed{0};
    uint64_t unmatched{0};
    uint64_t not_rtp{0};
    uint64_t port_full{0};

    auto operator==(ReceiveRefusals const&) const noexcept -> bool = default;
};

/// Counts refusals: count() on the real-time thread, load() from any thread (relaxed atomics).
class RefusalCounter
{
  public:
    void count(Refusal const refusal) noexcept { counts_[std::to_underlying(refusal)].fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] auto load() const noexcept -> ReceiveRefusals
    {
        return {
            .malformed = counts_[0].load(std::memory_order_relaxed),
            .unmatched = counts_[1].load(std::memory_order_relaxed),
            .not_rtp = counts_[2].load(std::memory_order_relaxed),
            .port_full = counts_[3].load(std::memory_order_relaxed),
        };
    }

  private:
    std::array<std::atomic<uint64_t>, 4> counts_{};
};

/// Datagrams addressed to one receiver's destination.
class ReceivePort
{
  public:
    virtual ~ReceivePort() = default;
    /// Up to `out.size()` datagrams that arrived since the last call; returns
    /// how many were written. Never blocks.
    [[nodiscard]] virtual auto receive(std::span<Datagram> out) noexcept -> size_t = 0;
};

/// Writes a packet's payload in place, exactly the length asked of send_rendered(), into the TX
/// buffer; false abandons the packet. Called on the real-time thread at most once per
/// send_rendered() call: a sender pulls its frames from it.
using PayloadRenderer = sg14::inplace_function<bool(std::span<uint8_t> payload), 64>;

/// RTP packets from one sender to its destination.
class SendPort
{
  public:
    virtual ~SendPort() = default;
    /// Queue a packet, copying `payload` in. False if it cannot be queued (the
    /// batch or the ring is full, or it does not fit a frame); the packet is
    /// then not sent.
    [[nodiscard]] virtual auto send(rtp::RtpHeader const& header, std::span<uint8_t const> payload) noexcept -> bool = 0;
    /// Queue a packet whose `payload_length`-byte payload `render` writes straight into the next TX
    /// buffer, after the headers; `render` is not called when no buffer can be reserved. True when
    /// queued; when `render` returns false or the packet is refused after it, the buffer goes unsent.
    [[nodiscard]] virtual auto send_rendered(
        rtp::RtpHeader const& header, size_t payload_length, PayloadRenderer const& render) noexcept -> bool = 0;
    /// As send_rendered(), the packet departing at @p departure_ns, the PTP time (ns) its sender sends it
    /// (0: untimed): a transport sending at launch times sends it then plus its lead; others ignore it.
    [[nodiscard]] virtual auto send_rendered_at(
        rtp::RtpHeader const& header,
        size_t const payload_length,
        PayloadRenderer const& render,
        uint64_t const departure_ns) noexcept -> bool
    {
        (void)departure_ns;
        return send_rendered(header, payload_length, render);
    }
    /// Send what send() and send_rendered() queued. Returns the number of packets the network
    /// refused (0 on success).
    virtual auto flush() noexcept -> size_t = 0;
};

/// Where a sender's packets go and how they are marked.
struct SendOptions
{
    Endpoint destination;
    uint8_t ttl{32};                                 ///< multicast TTL (unicast uses the host default)
    uint8_t dscp{rtp::FrameAddressing::DSCP_MEDIA};  ///< the IPv4 DSCP (Sender::set_dscp())
};

/// Where an RtcpPort's reports go and arrive, and how they are marked.
struct RtcpOptions
{
    /// Where reports go: a group (also where they arrive), or the unicast peer.
    Endpoint destination;
    /// Where reports arrive for a unicast destination: an address of this host (unspecified: the
    /// transport's) and port (0: destination.port). A group is its own local endpoint.
    Endpoint local{};
    uint8_t ttl{32};  ///< multicast TTL
    /// AES67-2018 Section 6.2: a sender's stream DSCP; a receiver's, the DSCP it would send a similar stream with.
    uint8_t dscp{rtp::FrameAddressing::DSCP_MEDIA};
};

/// One RTCP session's datagrams. Control or reactor thread only, never the real-time thread;
/// sends do not block, and both directions may make system calls.
class RtcpPort
{
  public:
    virtual ~RtcpPort() = default;
    /// A descriptor that polls readable when receive() has datagrams, or -1: call receive() each
    /// reactor cycle instead.
    [[nodiscard]] virtual auto fd() const noexcept -> int = 0;
    /// Up to `out.size()` datagrams that arrived since the last call; each payload is valid until
    /// the next call. Never blocks.
    [[nodiscard]] virtual auto receive(std::span<Datagram> out) noexcept -> size_t = 0;
    /// Send one compound packet to RtcpOptions::destination.
    [[nodiscard]] virtual auto send(std::span<uint8_t const> packet) noexcept -> Status = 0;
    /// Datagrams discarded before receive() (too long, or a full queue); any thread.
    [[nodiscard]] virtual auto dropped() const noexcept -> uint64_t { return 0; }
};

class Transport
{
  public:
    virtual ~Transport() = default;
    /// The interface's IPv4 address: the origin of every sender's SDP.
    [[nodiscard]] virtual auto local_address() const noexcept -> ip::IPv4Address = 0;
    /// Receive datagrams sent to `destination`, joining it if it is a group.
    [[nodiscard]] virtual auto open_receive(Endpoint const& destination) -> StatusValue<std::unique_ptr<ReceivePort>> = 0;
    /// Send to `options.destination`.
    [[nodiscard]] virtual auto open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>> = 0;
    /// RTCP to and from `options.destination`, joining it if it is a group. Not supported by default.
    [[nodiscard]] virtual auto open_rtcp(RtcpOptions const& options) -> StatusValue<std::unique_ptr<RtcpPort>>
    {
        (void)options;
        return failure(std::errc::operation_not_supported);
    }
    /// Before the receivers drain, each tick.
    virtual void begin_tick() noexcept {}
    /// After the senders send, each tick.
    virtual void end_tick() noexcept {}
    /// True when a port keeps datagrams only for the tick they arrived in
    /// (XDP), so receivers must drain every tick; false when it queues (UDP).
    [[nodiscard]] virtual auto drain_every_tick() const noexcept -> bool { return false; }
    /// Why the transport is not getting packets onto the wire (an AF_XDP
    /// kick the kernel keeps refusing), or none. Senders report it as their
    /// error. Real-time safe; any thread.
    [[nodiscard]] virtual auto send_error() const noexcept -> std::error_code { return {}; }
    /// Received frames refused before any receiver saw them, by reason. Any thread.
    [[nodiscard]] virtual auto receive_refusals() const noexcept -> ReceiveRefusals { return {}; }
};

}  // namespace statusbar::aes67
