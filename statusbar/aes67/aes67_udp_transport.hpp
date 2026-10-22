#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// AES67 over UDP sockets: one socket per receiver, bound to its destination
/// address and port (a group, joined on the configured interface, or one of
/// this host's addresses; one unicast receiver per address and port), with
/// IP_PKTINFO checking each datagram's destination, drained with recvmmsg(); one unconnected socket per sender (every message names the
/// destination, so a unicast receiver's ICMP port-unreachable never fails a
/// send), multicast out of the interface with the configured TTL and DSCP 46,
/// a tick's packets sent with sendmmsg().

#include "statusbar/aes67/aes67_socket.hpp"
#include "statusbar/aes67/aes67_transport.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace statusbar::aes67 {

class UdpTransport final : public Transport
{
  public:
    /// Datagrams one receive() takes at most.
    static constexpr size_t RECEIVE_BATCH = 32;
    /// Packets one sender may queue before flush().
    static constexpr size_t SEND_BATCH = 16;
    /// The largest datagram kept (an Ethernet MTU's worth).
    static constexpr size_t MAX_DATAGRAM = 1500;

    /// A transport on `interface`, which must have an IPv4 address.
    [[nodiscard]] static auto open(std::string_view interface) -> StatusValue<std::unique_ptr<UdpTransport>>;

    explicit UdpTransport(InterfaceInfo interface) noexcept
        : interface_{std::move(interface)}
    {}
    /// Every receive port must be gone first (asserted).
    ~UdpTransport() override;
    UdpTransport(UdpTransport const&) = delete;
    auto operator=(UdpTransport const&) -> UdpTransport& = delete;
    UdpTransport(UdpTransport&&) = delete;
    auto operator=(UdpTransport&&) -> UdpTransport& = delete;

    [[nodiscard]] auto local_address() const noexcept -> ip::IPv4Address override { return interface_.address; }
    [[nodiscard]] auto open_receive(Endpoint const& destination) -> StatusValue<std::unique_ptr<ReceivePort>> override;
    [[nodiscard]] auto open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>> override;

    /// A receive port closing (its destructor, on the control thread).
    void release(Endpoint const& destination) noexcept;

  private:
    InterfaceInfo interface_;
    std::vector<Endpoint> unicast_;  ///< destinations of the unicast receive ports (control thread)
    size_t live_ports_{0};           ///< receive ports not yet destroyed (control thread)
};

}  // namespace statusbar::aes67
