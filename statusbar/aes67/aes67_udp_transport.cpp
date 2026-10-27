// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_udp_transport.hpp"

#include "statusbar/aes67/aes67_error.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::aes67 {

namespace {

class UdpReceivePort final : public ReceivePort
{
  public:
    UdpReceivePort(
        UdpTransport& transport, net::FileDescriptor fd, Endpoint const& destination, unsigned int const interface_index) noexcept
        : transport_{transport}
        , fd_{std::move(fd)}
        , destination_{destination}
        , interface_index_{interface_index}
    {
        // Built once; receive() resets only what recvmmsg() writes back.
        for (size_t i = 0; i < UdpTransport::RECEIVE_BATCH; ++i) {
            iov_[i] = {.iov_base = buffers_[i].data(), .iov_len = buffers_[i].size()};
            messages_[i].msg_hdr.msg_name = &sources_[i];
            messages_[i].msg_hdr.msg_iov = &iov_[i];
            messages_[i].msg_hdr.msg_iovlen = 1;
            messages_[i].msg_hdr.msg_control = controls_[i].data();
        }
    }

    ~UdpReceivePort() override { transport_.release(destination_); }
    UdpReceivePort(UdpReceivePort const&) = delete;
    auto operator=(UdpReceivePort const&) -> UdpReceivePort& = delete;
    UdpReceivePort(UdpReceivePort&&) = delete;
    auto operator=(UdpReceivePort&&) -> UdpReceivePort& = delete;

    auto receive(std::span<Datagram> const out) noexcept -> size_t override
    {
        size_t const want = std::min(out.size(), UdpTransport::RECEIVE_BATCH);
        rearm(want);
        int const got = ::recvmmsg(fd_.get(), messages_.data(), static_cast<unsigned int>(want), MSG_DONTWAIT, nullptr);
        if (got <= 0) {
            return 0;
        }
        return take(static_cast<size_t>(got), out);
    }

  private:
    /// Resets the first `count` headers' lengths and flags for recvmmsg().
    void rearm(size_t const count) noexcept
    {
        for (size_t i = 0; i < count; ++i) {
            messages_[i].msg_len = 0;
            messages_[i].msg_hdr.msg_namelen = sizeof(sources_[i]);
            messages_[i].msg_hdr.msg_controllen = controls_[i].size();
            messages_[i].msg_hdr.msg_flags = 0;
        }
    }

    /// Hands the `got` datagrams read to `out`, skipping any truncated or for another address or interface.
    [[nodiscard]] auto take(size_t const got, std::span<Datagram> const out) noexcept -> size_t
    {
        size_t written = 0;
        for (size_t i = 0; i < got; ++i) {
            if ((messages_[i].msg_hdr.msg_flags & MSG_TRUNC) != 0 ||
                !destined_for(messages_[i].msg_hdr, destination_.address, interface_index_)) {
                continue;  // larger than any AES67 packet, or another address's or interface's
            }
            out[written++] = {
                .payload = std::span<uint8_t const>{buffers_[i]}.first(messages_[i].msg_len),
                .source = from_sockaddr(sources_[i]),
            };
        }
        return written;
    }

    UdpTransport& transport_;
    net::FileDescriptor fd_;
    Endpoint destination_;
    unsigned int interface_index_;  ///< the group's interface, or 0 for a unicast port
    std::array<std::array<uint8_t, UdpTransport::MAX_DATAGRAM>, UdpTransport::RECEIVE_BATCH> buffers_{};
    std::array<sockaddr_in, UdpTransport::RECEIVE_BATCH> sources_{};
    std::array<iovec, UdpTransport::RECEIVE_BATCH> iov_{};
    alignas(cmsghdr) std::array<std::array<uint8_t, PKTINFO_SPACE>, UdpTransport::RECEIVE_BATCH> controls_{};
    std::array<mmsghdr, UdpTransport::RECEIVE_BATCH> messages_{};
};

class UdpSendPort final : public SendPort
{
  public:
    UdpSendPort(net::FileDescriptor fd, sockaddr_in const& destination) noexcept
        : fd_{std::move(fd)}
        , destination_{destination}
    {}

    auto send(rtp::RtpHeader const& header, std::span<uint8_t const> const payload) noexcept -> bool override
    {
        if (queued_ == UdpTransport::SEND_BATCH) {
            return false;
        }
        auto const length = rtp::serialize_rtp(header, payload, buffers_[queued_]);
        if (!length) {
            return false;
        }
        enqueue(*length);
        return true;
    }

    auto send_rendered(rtp::RtpHeader const& header, size_t const payload_length, PayloadRenderer const& render) noexcept
        -> bool override
    {
        if (queued_ == UdpTransport::SEND_BATCH) {
            return false;
        }
        std::span<uint8_t> const buffer{buffers_[queued_]};
        auto const offset = rtp::serialize_rtp_header(header, payload_length, buffer);
        if (!offset || !render(buffer.subspan(*offset, payload_length))) {
            return false;  // the slot stays free: queued_ is not advanced
        }
        enqueue(*offset + payload_length);
        return true;
    }

    auto flush() noexcept -> size_t override
    {
        // sendmmsg() stops at the first message that fails: count that one
        // refused and carry on with the rest.
        size_t next = 0;
        size_t refused = 0;
        while (next < queued_) {
            int const n = ::sendmmsg(fd_.get(), &messages_[next], static_cast<unsigned int>(queued_ - next), MSG_DONTWAIT);
            if (n <= 0) {
                ++refused;
                ++next;
            } else {
                next += static_cast<size_t>(n);
            }
        }
        queued_ = 0;
        return refused;
    }

  private:
    /// Queue the datagram of `length` bytes written into the next batch slot.
    void enqueue(size_t const length) noexcept
    {
        iov_[queued_] = {.iov_base = buffers_[queued_].data(), .iov_len = length};
        messages_[queued_] = {
            .msg_hdr = {.msg_name = &destination_, .msg_namelen = sizeof(destination_), .msg_iov = &iov_[queued_], .msg_iovlen = 1},
            .msg_len = 0,
        };
        ++queued_;
    }

    net::FileDescriptor fd_;
    sockaddr_in destination_;  ///< every message's msg_name: unconnected, so no ICMP error reaches send
    size_t queued_{0};
    std::array<std::array<uint8_t, UdpTransport::MAX_DATAGRAM>, UdpTransport::SEND_BATCH> buffers_{};
    std::array<iovec, UdpTransport::SEND_BATCH> iov_{};
    std::array<mmsghdr, UdpTransport::SEND_BATCH> messages_{};
};

/// A receive buffer of 1 MiB, best effort, for a burst after a stall.
void set_receive_buffer(int const fd) noexcept
{
    int const buffer = 1 << 20;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
}

}  // namespace

auto UdpTransport::open(std::string_view const interface) -> StatusValue<std::unique_ptr<UdpTransport>>
{
    auto info = find_interface(interface);
    if (!info) {
        return failure(info.error());
    }
    return std::make_unique<UdpTransport>(std::move(*info));
}

UdpTransport::~UdpTransport()
{
    assert(live_ports_ == 0 && "every receive port must be destroyed before its transport");
}

auto UdpTransport::open_receive(Endpoint const& destination) -> StatusValue<std::unique_ptr<ReceivePort>>
{
    if (!destination.address.is_multicast() && std::ranges::find(unicast_, destination) != unicast_.end()) {
        return failure(std::errc::address_in_use);  // one unicast receiver per address and port
    }
    // Bound to the destination address itself: a group's socket sees only that group, and a
    // unicast receiver's specific bind wins its address:port over the group sockets on the port.
    auto fd = open_receive_socket(interface_, destination, true);
    if (!fd) {
        return failure(fd.error());
    }
    set_receive_buffer(fd->get());
    retain(destination);
    return std::make_unique<UdpReceivePort>(
        *this, std::move(*fd), destination, destination.address.is_multicast() ? interface_.index : 0U);
}

void UdpTransport::retain(Endpoint const& destination)
{
    if (!destination.address.is_multicast()) {
        unicast_.push_back(destination);
    }
    ++live_ports_;
}

void UdpTransport::release(Endpoint const& destination) noexcept
{
    if (!destination.address.is_multicast()) {
        if (auto const it = std::ranges::find(unicast_, destination); it != unicast_.end()) {
            unicast_.erase(it);
        }
    }
    --live_ports_;
}

auto UdpTransport::open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>>
{
    auto fd = net::make_udp_socket();
    if (!fd) {
        return failure(fd.error());
    }
    if (options.destination.address.is_multicast()) {
        if (auto set = net::set_multicast_send(fd->get(), interface_.index, options.ttl, true); !set) {
            return failure(set.error());
        }
    }
    if (auto set = net::set_dscp(fd->get(), AF_INET, options.dscp); !set) {
        return failure(set.error());
    }
    // Not connected: a connected UDP socket reports a unicast destination's
    // ICMP port-unreachable as ECONNREFUSED on the next send.
    return std::make_unique<UdpSendPort>(std::move(*fd), to_sockaddr(options.destination));
}

auto UdpTransport::open_rtcp(RtcpOptions const& options) -> StatusValue<std::unique_ptr<RtcpPort>>
{
    return open_rtcp_socket(interface_, options);
}

}  // namespace statusbar::aes67
