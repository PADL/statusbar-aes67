#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// How AES67 receivers and senders reach the network: a Transport opens a
/// ReceivePort per receiver (joined to its group) and a SendPort per sender.
/// Two implementations: UdpTransport (sockets, any host) and XdpTransport
/// (AF_XDP raw frames on one NIC queue).
///
/// Threads and lifetimes, the contract every transport keeps:
/// - The control thread (one thread) calls open_receive() / open_send() and
///   destroys the ports. These may allocate and make syscalls.
/// - The real-time thread calls ReceivePort::receive(), SendPort::send(),
///   send_rendered() and flush(), and begin_tick() / end_tick(): no
///   allocation, no blocking, no logging. Each tick is bracketed by
///   begin_tick() (before any receiver drains) and end_tick() (after every
///   sender has sent): the XDP transport drains its shared RX ring in
///   begin_tick() and kicks the NIC in end_tick(); for UDP both do nothing.
/// - Opening and destroying ports is safe while the real-time thread ticks the
///   transport: a transport that shares state between ports (XDP) hands port
///   changes to the real-time thread through queues and applies them in
///   begin_tick(), freeing a closed port's state only after the real-time
///   thread has let it go.
/// - A port must not be destroyed while the real-time thread may still call
///   that port (the owner swaps it out between ticks and retires it first, as
///   the flow engine does).
/// - The transport outlives every port it opened (asserted in its destructor),
///   and the real-time thread stops ticking it before it is destroyed.

#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/rtp/rtp_header.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>

namespace statusbar::aes67 {

/// An IPv4 address and UDP port.
struct Endpoint
{
    ip::IPv4Address address;
    uint16_t port{0};

    auto operator==(Endpoint const&) const noexcept -> bool = default;
};

/// A received UDP payload. `payload` is valid until the next receive() on the
/// same port or the next begin_tick(), whichever is first.
struct Datagram
{
    std::span<uint8_t const> payload;
    Endpoint source;
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

/// Writes a packet's payload in place: `payload` is exactly the length the
/// caller asked send_rendered() for, inside the transport's TX buffer. False
/// abandons the packet. Runs on the real-time thread, inside send_rendered(),
/// at most once per call: a sender pulls its frames from it, so a transport
/// must never call it again for the same packet.
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
    /// Queue a packet whose payload of `payload_length` bytes `render` writes
    /// straight into the transport's next TX buffer (a sendmmsg() batch slot, a
    /// UMEM frame), after the headers, so the payload is copied once, by
    /// whatever renders it. The buffer is reserved first: when it cannot be
    /// (as send() fails) `render` is not called. When `render` returns false,
    /// or the packet is refused after it ran (the XDP TX ring is full), the
    /// buffer is released unsent: a batch slot is not counted, a UMEM frame
    /// goes back to the pool (or, refused by the ring, is freed). Returns true
    /// when the packet is queued, as send() does; flush() as for send().
    [[nodiscard]] virtual auto send_rendered(
        rtp::RtpHeader const& header, size_t payload_length, PayloadRenderer const& render) noexcept -> bool = 0;
    /// Send what send() and send_rendered() queued. Returns the number of packets the network
    /// refused (0 on success).
    virtual auto flush() noexcept -> size_t = 0;
};

/// Where a sender's packets go and how they are marked.
struct SendOptions
{
    Endpoint destination;
    uint8_t ttl{32};   ///< multicast TTL (unicast uses the host default)
    uint8_t dscp{46};  ///< Expedited Forwarding, AES67 section 6.1
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
};

}  // namespace statusbar::aes67
