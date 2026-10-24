#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// How AES67 receivers and senders reach the network: a Transport opens a ReceivePort per receiver
/// and a SendPort per sender. The control thread opens and destroys ports, safely while the
/// real-time thread ticks; the real-time thread brackets each tick with begin_tick() / end_tick().

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
    /// Send what send() and send_rendered() queued. Returns the number of packets the network
    /// refused (0 on success).
    virtual auto flush() noexcept -> size_t = 0;
};

/// Where a sender's packets go and how they are marked.
struct SendOptions
{
    Endpoint destination;
    uint8_t ttl{32};   ///< multicast TTL (unicast uses the host default)
    uint8_t dscp{46};  ///< Expedited Forwarding, AES67 §6.1
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
