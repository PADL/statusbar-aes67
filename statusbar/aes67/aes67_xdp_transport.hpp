#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// AES67 over AF_XDP: one XdpContext on one NIC queue serves every receiver and
/// sender. The XDP program redirects to the socket the untagged IPv4 (IHL 5)
/// UDP frames whose destination is exactly one of the joined groups (the rule
/// matches the group's MAC and the address's first two octets, which together
/// are the whole address), and passes everything else to the kernel.
/// begin_tick() drains the RX ring once and hands each frame, parsed with
/// rtp::parse_rtp_frame(), to the ports for its destination group and port;
/// senders write whole frames with rtp::serialize_rtp_frame() into UMEM, and
/// end_tick() kicks the NIC.
///
/// Group membership is still the kernel's: each receive port holds a plain UDP
/// socket joined to its group, so IGMP reports go out and the NIC's multicast
/// filter keeps the group.
///
/// Ports and threads (see aes67_transport.hpp for the contract): open_receive()
/// and a receive port's destruction run on the control thread and only post
/// to the real-time thread: a new port goes through a bounded queue, a closed
/// one is marked closed. begin_tick() is the only place the port list and the
/// installed rules change: it takes new ports, drops closed ones (handing
/// their state back on a retire queue, freed on the control thread's next
/// open_*() or port destruction, or by the transport's destructor), and
/// reinstalls the rules when the set changed (a BPF map update: a syscall on
/// the real-time thread, only when receivers come or go).
///
/// UMEM: senders keep TX_POOL frames back, topped up each tick before the
/// fill ring is; those are simply never offered to the kernel. The rest of
/// the UMEM backs the fill ring (FILL_RING entries) and the frames in flight.
///
/// Limits: multicast destinations only, both ways (a unicast receiver would
/// need a rule stealing the host's UDP, a unicast sender ARP); at most
/// MAX_GROUPS distinct groups (the BPF program's TCAM holds 16 rules); a
/// joined group's frames to ports no receiver here listens on do not reach the
/// kernel (the rule cannot see the UDP port), so other listeners on the same
/// group miss them; untagged frames only; one queue (queue 0), so the NIC must
/// deliver the groups there (one combined channel, or a flow-steering rule).

#include "statusbar/aes67/aes67_socket.hpp"
#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/itc/itc_message_pipe.hpp"
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/net/net_compiled_rule.hpp"
#include "statusbar/net/net_linux_xdp.hpp"
#include "statusbar/rtp/rtp_frame.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace statusbar::aes67 {

/// What XdpTransport::open() takes past the interface (outside the class so it
/// can be a defaulted argument there).
struct XdpTransportOptions
{
    /// Zero-copy or copy bind; auto_ lets the kernel pick (zero-copy when the
    /// driver offers it).
    net::XdpBindMode bind_mode{net::XdpBindMode::auto_};
};

class XdpTransport final : public Transport
{
  public:
    /// Frames begin_tick() takes from the RX ring at most (XdpConfig::rx_drain_max).
    static constexpr size_t RECEIVE_BATCH = 64;
    /// Frames one port may hold for receive() between ticks.
    static constexpr size_t PORT_BATCH = 32;
    /// UMEM frames held back for senders.
    static constexpr size_t TX_POOL = 64;
    /// The fill ring's entries.
    static constexpr size_t FILL_RING = 1024;
    /// UMEM frames: the fill ring's worth plus as many again for frames in the
    /// RX ring, held for receivers, in the TX pool and in flight.
    static constexpr size_t UMEM_FRAMES = 2 * FILL_RING;
    /// Distinct groups the receivers may join.
    static constexpr size_t MAX_GROUPS = 16;
    /// Receive ports at most (several may share a group).
    static constexpr size_t MAX_PORTS = 64;

    /// Frames held back for senders (internal; public for tests).
    class TxPool
    {
      public:
        [[nodiscard]] auto take() noexcept -> std::optional<net::EthernetTxSlot>
        {
            if (count_ == 0) {
                return std::nullopt;
            }
            return slots_[--count_];
        }
        /// False when full.
        auto put(net::EthernetTxSlot const& slot) noexcept -> bool
        {
            if (count_ == TX_POOL) {
                return false;
            }
            slots_[count_++] = slot;
            return true;
        }
        [[nodiscard]] auto size() const noexcept -> size_t { return count_; }

      private:
        std::array<net::EthernetTxSlot, TX_POOL> slots_{};
        size_t count_{0};
    };

    /// Write an RTP frame into a pooled TX frame: the slot and the frame
    /// length, or nullopt (the pool was empty, or the frame did not fit, and
    /// the slot went back to the pool).
    [[nodiscard]] static auto write_frame(
        TxPool& pool,
        rtp::FrameAddressing const& addressing,
        rtp::RtpHeader const& header,
        std::span<uint8_t const> payload) noexcept -> std::optional<std::pair<net::EthernetTxSlot, size_t>>;

    using Options = XdpTransportOptions;

    /// The XdpConfig open() uses on `interface`: UMEM_FRAMES frames, a
    /// FILL_RING fill ring, rx_drain_max RECEIVE_BATCH, queue 0, no rules yet,
    /// and `options`' bind mode.
    [[nodiscard]] static auto config_for(std::string_view interface, Options const& options = {}) noexcept -> net::XdpConfig;

    /// The rule redirecting `group`'s frames to the socket.
    [[nodiscard]] static auto group_rule(ip::IPv4Address const& group) noexcept -> net::CompiledRule<32>;

    /// Open AF_XDP on `interface`'s queue 0. Fails without a supported NIC,
    /// libxdp, or the privileges (CAP_NET_ADMIN, CAP_BPF / root), and with
    /// bind_mode zero_copy when the driver offers no zero-copy.
    [[nodiscard]] static auto open(std::string_view interface, Options const& options = {})
        -> StatusValue<std::unique_ptr<XdpTransport>>;

    /// Every receive port must be gone first (asserted); the real-time thread
    /// must have stopped ticking the transport.
    ~XdpTransport() override;
    XdpTransport(XdpTransport const&) = delete;
    auto operator=(XdpTransport const&) -> XdpTransport& = delete;
    XdpTransport(XdpTransport&&) = delete;
    auto operator=(XdpTransport&&) -> XdpTransport& = delete;

    [[nodiscard]] auto local_address() const noexcept -> ip::IPv4Address override { return interface_.address; }
    /// The interface the socket is bound on.
    [[nodiscard]] auto interface_name() const noexcept -> std::string_view { return interface_.name; }
    /// True when the socket got zero-copy (read back after the bind), false in copy mode.
    [[nodiscard]] auto zero_copy() const noexcept -> bool { return context_.zero_copy(); }
    /// The bind mode open() asked for.
    [[nodiscard]] auto bind_mode() const noexcept -> net::XdpBindMode { return context_.bind_mode(); }
    /// Fails with too_many_filter_rules past MAX_GROUPS groups, and with
    /// resource_unavailable_try_again past MAX_PORTS ports or when the queue to
    /// the real-time thread is full.
    [[nodiscard]] auto open_receive(Endpoint const& destination) -> StatusValue<std::unique_ptr<ReceivePort>> override;
    [[nodiscard]] auto open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>> override;
    void begin_tick() noexcept override;
    void end_tick() noexcept override;

    /// The last rule installation's result (made by begin_tick()); any thread.
    [[nodiscard]] auto rules_status() const noexcept -> Status;

    struct PortState;  ///< a receive port's state (internal)
    class Port;        ///< a receive port (internal)
    class Sender;      ///< a send port (internal)

  private:
    friend class Port;
    friend class Sender;

    explicit XdpTransport(InterfaceInfo interface) noexcept;

    /// Control thread: a port's handle is gone.
    void port_closed(PortState* state) noexcept;
    /// Control thread: free what the real-time thread retired.
    void collect_retired() noexcept;
    /// Real-time thread: install a rule per distinct group of ports_, without allocating.
    void install_rules() noexcept;
    void release_held() noexcept;
    void refill_tx_pool() noexcept;

    InterfaceInfo interface_;
    net::XdpContext context_;

    // Control thread only.
    std::vector<std::pair<ip::IPv4Address, size_t>> groups_;  ///< groups of the open ports, with counts
    size_t live_ports_{0};

    // Between the threads.
    itc::QueuedPipe<PortState*, 128> added_;    ///< control -> real-time
    itc::QueuedPipe<PortState*, 128> retired_;  ///< real-time -> control
    itc::SeqlockValue<std::error_code> rules_error_;

    // Real-time thread only.
    std::array<PortState*, MAX_PORTS> ports_{};
    size_t port_count_{0};
    std::array<uint64_t, RECEIVE_BATCH> held_{};
    size_t held_count_{0};
    TxPool tx_pool_;
    std::array<net::CompiledRule<32>, MAX_GROUPS> rules_{};
};

}  // namespace statusbar::aes67
