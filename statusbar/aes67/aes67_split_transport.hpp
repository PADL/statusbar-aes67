#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// Two transports on one interface as one, split by destination: groups go to the multicast transport (AF_XDP),
/// unicast addresses to the unicast one (kernel sockets, which resolve neighbours and route). Ports, threads and
/// the tick bracket as aes67_transport.hpp; each tick brackets both transports.

#include "statusbar/aes67/aes67_transport.hpp"

#include <memory>

namespace statusbar::aes67 {

class SplitTransport final : public Transport
{
  public:
    /// Both transports on the same interface (same local_address()); neither null.
    SplitTransport(std::unique_ptr<Transport> multicast, std::unique_ptr<Transport> unicast) noexcept
        : multicast_{std::move(multicast)}
        , unicast_{std::move(unicast)}
    {}
    ~SplitTransport() override = default;
    SplitTransport(SplitTransport const&) = delete;
    auto operator=(SplitTransport const&) -> SplitTransport& = delete;
    SplitTransport(SplitTransport&&) = delete;
    auto operator=(SplitTransport&&) -> SplitTransport& = delete;

    /// The transport groups go to, and the one unicast addresses go to.
    [[nodiscard]] auto multicast() const noexcept -> Transport& { return *multicast_; }
    [[nodiscard]] auto unicast() const noexcept -> Transport& { return *unicast_; }

    /// The interface's, as the unicast transport (the one that binds this host's addresses) says.
    [[nodiscard]] auto local_address() const noexcept -> ip::IPv4Address override { return unicast_->local_address(); }
    [[nodiscard]] auto is_local_address(ip::IPv4Address const& address) const noexcept -> bool override
    {
        return unicast_->is_local_address(address);
    }
    [[nodiscard]] auto open_receive(Endpoint const& destination) -> StatusValue<std::unique_ptr<ReceivePort>> override
    {
        return for_address(destination.address).open_receive(destination);
    }
    /// A unicast destination's port is untagged whatever `options.vlan` says (IEEE 802.1Q-2022 Clause 35.2.2.8.3).
    [[nodiscard]] auto open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>> override;
    [[nodiscard]] auto open_rtcp(RtcpOptions const& options) -> StatusValue<std::unique_ptr<RtcpPort>> override
    {
        return for_address(options.destination.address).open_rtcp(options);
    }
    void begin_tick() noexcept override;
    void end_tick() noexcept override;
    [[nodiscard]] auto drain_every_tick(Endpoint const& destination) const noexcept -> bool override
    {
        return for_address(destination.address).drain_every_tick(destination);
    }
    /// The multicast transport's, else the unicast one's.
    [[nodiscard]] auto send_error() const noexcept -> std::error_code override;
    /// The error of the transport `destination` goes by.
    [[nodiscard]] auto send_error_for(Endpoint const& destination) const noexcept -> std::error_code override
    {
        return for_address(destination.address).send_error_for(destination);
    }
    /// Both transports' refusals, summed.
    [[nodiscard]] auto receive_refusals() const noexcept -> ReceiveRefusals override;
    void reset_counters() noexcept override;
    /// The multicast transport's: its ports alone keep and gather frames; a unicast port refuses both and copies.
    [[nodiscard]] auto holds_frames() const noexcept -> bool override { return multicast_->holds_frames(); }
    /// Given back to the multicast transport, whose ports alone hold.
    void release_frame(uint64_t const frame) noexcept override { multicast_->release_frame(frame); }
    [[nodiscard]] auto frames_shared() const noexcept -> bool override { return multicast_->frames_shared(); }
    [[nodiscard]] auto gather_buffers() const noexcept -> size_t override { return multicast_->gather_buffers(); }
    [[nodiscard]] auto writes_headers_before() const noexcept -> bool override { return multicast_->writes_headers_before(); }

  private:
    [[nodiscard]] auto for_address(ip::IPv4Address const& address) const noexcept -> Transport&
    {
        return address.is_multicast() ? *multicast_ : *unicast_;
    }

    std::unique_ptr<Transport> multicast_;
    std::unique_ptr<Transport> unicast_;
};

}  // namespace statusbar::aes67
