#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// AES67 over AF_XDP: one XdpContext on NIC queue 0 serves every receiver and sender; the XDP
/// program redirects the joined groups' untagged IPv4/UDP frames and passes the rest to the kernel.
/// Multicast destinations only, at most MAX_GROUPS groups; ports and threads as aes67_transport.hpp.
/// RTCP goes by kernel socket, except to a group whose frames the program redirects: begin_tick()
/// copies those into the port's RtcpPipe.

#include "statusbar/aes67/aes67_socket.hpp"
#include "statusbar/aes67/aes67_transport.hpp"
#include "statusbar/itc/itc_message_pipe.hpp"
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/net/net_compiled_rule.hpp"
#include "statusbar/net/net_linux_xdp.hpp"
#include "statusbar/net/net_rss.hpp"
#include "statusbar/rtp/rtp_frame.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace statusbar::aes67 {

class XdpRtcpPort;

/// What XdpTransport::open() takes past the interface (outside the class so it
/// can be a defaulted argument there).
struct XdpTransportOptions
{
    /// Zero-copy or copy bind (net::XdpBindMode).
    net::XdpBindMode bind_mode{net::XdpBindMode::auto_};
    /// How a fresh attach attaches the program (net::XdpConfig::attach_mode).
    net::XdpAttachMode attach_mode{net::XdpAttachMode::native};
    /// Where the program is pinned and adopted from (net::XdpConfig::pin_path):
    /// empty for none, or net::XdpProgram::pin_path_for(interface).
    std::string pin_path;
    /// Steer every RSS bucket to the socket's queue at open (net::set_rss_indirection_all): with
    /// RSS spreading received UDP over the NIC's queues, most groups would miss the socket.
    bool steer_rss{true};
};

class XdpTransport final : public Transport
{
  public:
    /// The NIC queue the socket binds.
    static constexpr uint32_t QUEUE = 0;
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
    /// Consecutive refused TX kicks that put the transport in error (1 ms of ticks).
    static constexpr uint32_t TX_FAILING_AFTER = 8;
    /// RTCP ports at most.
    static constexpr size_t MAX_RTCP_PORTS = 64;

    /// The queue that carries an RTCP port's redirected datagrams from begin_tick() to the
    /// reactor, copied out of the UMEM (internal; public for tests).
    class RtcpPipe
    {
      public:
        /// Datagrams held at most (a 16-slot ring); more are dropped and counted.
        static constexpr size_t DEPTH = 15;
        /// The longest datagram kept, as long as a kernel socket's; a longer one is dropped and counted.
        static constexpr size_t MAX_DATAGRAM = 1500;

        /// A copied datagram; only `length` bytes are written.
        struct Frame
        {
            std::array<uint8_t, MAX_DATAGRAM> bytes;
            uint16_t length{0};
            Endpoint source;
        };

        explicit RtcpPipe(Endpoint const& destination) noexcept
            : destination_{destination}
        {}

        /// The RTCP endpoint (group and port) whose datagrams this pipe takes.
        [[nodiscard]] auto destination() const noexcept -> Endpoint const& { return destination_; }
        /// Real-time thread: copy `datagram` in; false, and counted, when too long or full.
        auto offer(Datagram const& datagram) noexcept -> bool;
        /// Reactor: move up to `storage.size()` datagrams into `storage` and view them in `out`
        /// (as long as `storage`); returns how many.
        auto take(std::span<Datagram> out, std::span<Frame> storage) noexcept -> size_t;
        /// Datagrams offer() refused. Any thread.
        [[nodiscard]] auto dropped() const noexcept -> uint64_t { return dropped_.load(std::memory_order_relaxed); }

        std::atomic<bool> closed{false};  ///< set when the port is gone (control thread)

      private:
        Endpoint destination_;
        itc::QueuedPipe<Frame, DEPTH + 1> queue_;
        std::atomic<uint64_t> dropped_{0};  ///< written by the real-time thread only
    };

    /// Frames held back for senders (internal; public for tests).
    class TxPool
    {
      public:
        /// Counts a miss (empty_count()) when empty.
        [[nodiscard]] auto take() noexcept -> std::optional<net::EthernetTxSlot>
        {
            if (count_ == 0) {
                empty_.store(empty_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
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
        /// take() calls that found the pool empty. Any thread.
        [[nodiscard]] auto empty_count() const noexcept -> uint64_t { return empty_.load(std::memory_order_relaxed); }

      private:
        std::array<net::EthernetTxSlot, TX_POOL> slots_{};
        size_t count_{0};
        std::atomic<uint64_t> empty_{0};  ///< written by the owning thread only
    };

    /// Write an RTP frame into a pooled TX frame: the slot and the frame
    /// length, or nullopt (the pool was empty, or the frame did not fit, and
    /// the slot went back to the pool).
    [[nodiscard]] static auto write_frame(
        TxPool& pool,
        rtp::FrameAddressing const& addressing,
        rtp::RtpHeader const& header,
        std::span<uint8_t const> payload) noexcept -> std::optional<std::pair<net::EthernetTxSlot, size_t>>;

    /// Write an RTP frame's headers into a pooled TX frame and have `render` write the payload
    /// after them: the slot and the frame length, or nullopt (pool empty: `render` not called;
    /// frame too long or `render` false: the slot back in the pool).
    [[nodiscard]] static auto render_frame(
        TxPool& pool,
        rtp::FrameAddressing const& addressing,
        rtp::RtpHeader const& header,
        size_t payload_length,
        PayloadRenderer const& render) noexcept -> std::optional<std::pair<net::EthernetTxSlot, size_t>>;

    using Options = XdpTransportOptions;

    /// The XdpConfig open() uses on `interface`: UMEM_FRAMES frames, a
    /// FILL_RING fill ring, queue 0, no rules yet, and `options` (its pin path
    /// by view: `options` must outlive the config).
    [[nodiscard]] static auto config_for(std::string_view interface, Options const& options = {}) noexcept -> net::XdpConfig;

    /// The rule redirecting `group`'s frames to the socket.
    [[nodiscard]] static auto group_rule(ip::IPv4Address const& group) noexcept -> net::CompiledRule<32>;

    /// Open AF_XDP on `interface`'s queue 0, adopting or attaching (and
    /// pinning) the program as net::XdpConfig says, then steer RSS (Options::steer_rss).
    /// Fails without a supported NIC, libxdp or the privileges, or zero-copy when it was required.
    [[nodiscard]] static auto open(std::string_view interface, Options const& options = {})
        -> StatusValue<std::unique_ptr<XdpTransport>>;

    /// Every receive and RTCP port must be gone first (asserted); the real-time
    /// thread must have stopped ticking the transport.
    ~XdpTransport() override;
    XdpTransport(XdpTransport const&) = delete;
    auto operator=(XdpTransport const&) -> XdpTransport& = delete;
    XdpTransport(XdpTransport&&) = delete;
    auto operator=(XdpTransport&&) -> XdpTransport& = delete;

    [[nodiscard]] auto local_address() const noexcept -> ip::IPv4Address override { return interface_.address; }
    /// The interface the socket is bound on.
    [[nodiscard]] auto interface_name() const noexcept -> std::string_view { return interface_.name; }
    /// True when the socket runs zero-copy: the program is native and the
    /// bind read back zero-copy; false in copy mode.
    [[nodiscard]] auto zero_copy() const noexcept -> bool { return context_.zero_copy(); }
    /// The bind mode open() asked for.
    [[nodiscard]] auto bind_mode() const noexcept -> net::XdpBindMode { return context_.bind_mode(); }
    /// The mode the program runs in.
    [[nodiscard]] auto attach_mode() const noexcept -> net::XdpAttachMode { return context_.attach_mode(); }
    /// True when open() adopted a pinned program (no attach, no adapter reset).
    [[nodiscard]] auto adopted() const noexcept -> bool { return context_.adopted(); }
    /// True when the program is pinned (adopted from, or pinned at, Options::pin_path).
    [[nodiscard]] auto pinned() const noexcept -> bool { return context_.program().pinned(); }
    /// The queue the socket is bound to.
    [[nodiscard]] auto queue_id() const noexcept -> uint32_t { return QUEUE; }
    /// open()'s RSS steering: nullopt when Options::steer_rss was false; success when every bucket
    /// goes to queue_id(); else why not (operation_not_supported: the NIC has no RSS).
    [[nodiscard]] auto rss_status() const noexcept -> std::optional<Status> { return rss_status_; }
    /// Refused TX kicks, the last errno, and whether the transport is failing
    /// (internal; public for tests). record() on the real-time thread only.
    class TxHealth
    {
      public:
        /// One kick's result: refused with `error` (an errno) unless `ok`.
        void record(bool const ok, int const error) noexcept
        {
            if (ok) {
                in_a_row_ = 0;
                if (failing_.load(std::memory_order_relaxed)) {
                    failing_.store(false, std::memory_order_relaxed);
                }
                return;
            }
            errno_.store(error, std::memory_order_relaxed);
            failures_.fetch_add(1, std::memory_order_relaxed);
            if (++in_a_row_ >= TX_FAILING_AFTER && !failing_.load(std::memory_order_relaxed)) {
                failing_.store(true, std::memory_order_relaxed);
            }
        }
        [[nodiscard]] auto failures() const noexcept -> uint64_t { return failures_.load(std::memory_order_relaxed); }
        /// The last refusal's errno as an error code (none before the first).
        [[nodiscard]] auto error() const noexcept -> std::error_code
        {
            int const value = errno_.load(std::memory_order_relaxed);
            return value != 0 ? std::error_code{value, std::system_category()} : std::error_code{};
        }
        /// TX_FAILING_AFTER refusals in a row, and none accepted since.
        [[nodiscard]] auto failing() const noexcept -> bool { return failing_.load(std::memory_order_relaxed); }

      private:
        uint32_t in_a_row_{0};  ///< real-time thread
        std::atomic<uint64_t> failures_{0};
        std::atomic<int> errno_{0};
        std::atomic<bool> failing_{false};
    };

    /// TX kicks the kernel refused since open(); any thread.
    [[nodiscard]] auto tx_flush_failures() const noexcept -> uint64_t { return tx_health_.failures(); }
    /// The last refused kick's errno, as an error code (none before the
    /// first); any thread.
    [[nodiscard]] auto tx_flush_error() const noexcept -> std::error_code { return tx_health_.error(); }
    /// While TX_FAILING_AFTER kicks in a row have been refused (and until one
    /// is not): tx_flush_error(); else none.
    [[nodiscard]] auto send_error() const noexcept -> std::error_code override
    {
        return tx_health_.failing() ? tx_health_.error() : std::error_code{};
    }
    /// The socket's counters as of the last end_tick() (rx_count and tx_count are the
    /// frames received and sent; tx_flush_failures() the ticks whose kick failed). Any thread.
    [[nodiscard]] auto socket_stats() const noexcept -> net::XdpSocketStats { return socket_stats_.load(); }
    /// Sends that found no pooled TX frame. Any thread.
    [[nodiscard]] auto tx_pool_empty_count() const noexcept -> uint64_t { return tx_pool_.empty_count(); }
    /// Redirected frames no receive port kept, by reason (RTCP pipes' drops are theirs). Any thread.
    [[nodiscard]] auto receive_refusals() const noexcept -> ReceiveRefusals override { return refusals_.load(); }
    /// A port's datagrams last only the tick they arrived in.
    [[nodiscard]] auto drain_every_tick() const noexcept -> bool override { return true; }
    /// Fails with too_many_filter_rules past MAX_GROUPS groups, and with
    /// resource_unavailable_try_again past MAX_PORTS ports or when the queue to
    /// the real-time thread is full.
    [[nodiscard]] auto open_receive(Endpoint const& destination) -> StatusValue<std::unique_ptr<ReceivePort>> override;
    [[nodiscard]] auto open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>> override;
    /// A kernel socket as UdpTransport's for sending and for a group the program passes, and an
    /// RtcpPipe for when it redirects the group; fd() is -1. Groups only (unsupported_destination);
    /// resource_unavailable_try_again past MAX_RTCP_PORTS or when the queue to the real-time thread is full.
    [[nodiscard]] auto open_rtcp(RtcpOptions const& options) -> StatusValue<std::unique_ptr<RtcpPort>> override;
    void begin_tick() noexcept override;
    void end_tick() noexcept override;

    /// The last rule installation's result (made by begin_tick()); any thread.
    [[nodiscard]] auto rules_status() const noexcept -> Status;

    struct PortState;  ///< a receive port's state (internal; public for tests)
    class Port;        ///< a receive port (internal)
    class Sender;      ///< a send port (internal)

    /// Offers one received Ethernet frame to `ports` and `pipes`, as begin_tick() does, counting a
    /// refusal in `refusals`; true when a port kept it, to be held until the next tick (internal; public for tests).
    [[nodiscard]] static auto dispatch_frame(
        std::span<uint8_t const> frame,
        std::span<PortState* const> ports,
        std::span<RtcpPipe* const> pipes,
        RefusalCounter& refusals) noexcept -> bool;

  private:
    friend class Port;
    friend class Sender;
    friend class aes67::XdpRtcpPort;

    explicit XdpTransport(InterfaceInfo interface) noexcept;

    /// Control thread: a port's handle is gone.
    void port_closed(PortState* state) noexcept;
    /// Control thread: free what the real-time thread retired.
    void collect_retired() noexcept;
    /// Control thread: an RTCP port's handle is gone.
    void rtcp_closed(RtcpPipe* pipe) noexcept;
    /// Real-time thread: take new RTCP pipes, retire closed ones.
    void update_rtcp_pipes() noexcept;
    /// Real-time thread: install a rule per distinct group of ports_, without allocating.
    void install_rules() noexcept;
    void release_held() noexcept;
    void refill_tx_pool() noexcept;
    /// Destructor: frees every port state and RTCP pipe, live or not yet taken.
    void delete_port_states() noexcept;
    /// Control thread: why a receive port for `destination` cannot open, if it cannot.
    [[nodiscard]] auto check_receive(Endpoint const& destination) const noexcept -> Status;
    /// Control thread: a socket that joins `group`, so the switch sends it here.
    [[nodiscard]] auto open_group_socket(ip::IPv4Address const& group) const -> StatusValue<net::FileDescriptor>;
    /// Control thread: counts one more port on `group`.
    void retain_group(ip::IPv4Address const& group);
    /// Control thread: a new port's state, handed to the real-time thread.
    [[nodiscard]] auto publish_port(Endpoint const& destination, net::FileDescriptor fd) -> PortState*;
    /// Control thread: a new RTCP pipe, handed to the real-time thread.
    [[nodiscard]] auto publish_rtcp_pipe(Endpoint const& destination) -> RtcpPipe*;
    /// Real-time thread: take new ports, retire closed ones; true when the set changed.
    [[nodiscard]] auto update_ports() noexcept -> bool;
    /// Real-time thread: reap TX completions, refill the TX pool and the fill ring, begin the cycle.
    void start_cycle() noexcept;
    /// Real-time thread: take the received frames, holding those a receive port kept.
    void receive_frames() noexcept;

    InterfaceInfo interface_;
    net::XdpContext context_;
    std::optional<Status> rss_status_;

    // Control thread only.
    std::vector<std::pair<ip::IPv4Address, size_t>> groups_;  ///< groups of the open ports, with counts
    size_t live_ports_{0};
    size_t live_rtcp_{0};

    // Between the threads.
    itc::QueuedPipe<PortState*, 128> added_;        ///< control -> real-time
    itc::QueuedPipe<PortState*, 128> retired_;      ///< real-time -> control
    itc::QueuedPipe<RtcpPipe*, 128> rtcp_added_;    ///< control -> real-time
    itc::QueuedPipe<RtcpPipe*, 128> rtcp_retired_;  ///< real-time -> control
    itc::SeqlockValue<std::error_code> rules_error_;
    itc::SeqlockValue<net::XdpSocketStats> socket_stats_;  ///< real-time -> any

    // Real-time thread only.
    std::array<PortState*, MAX_PORTS> ports_{};
    size_t port_count_{0};
    std::array<RtcpPipe*, MAX_RTCP_PORTS> rtcp_pipes_{};
    size_t rtcp_count_{0};
    std::array<uint64_t, RECEIVE_BATCH> held_{};
    size_t held_count_{0};
    TxPool tx_pool_;
    std::array<net::CompiledRule<32>, MAX_GROUPS> rules_{};
    TxHealth tx_health_;
    RefusalCounter refusals_;  ///< real-time -> any
};

/// A receive port's state: made on the control thread, used on the real-time thread from the
/// tick that takes it until the tick that retires it, freed on the control thread.
struct XdpTransport::PortState
{
    PortState(Endpoint const& d, net::FileDescriptor m) noexcept
        : destination{d}
        , membership{std::move(m)}
    {}

    Endpoint destination;
    net::FileDescriptor membership;  ///< keeps the kernel (and the NIC filter) joined
    std::array<Datagram, PORT_BATCH> pending{};
    size_t pending_count{0};
    std::atomic<bool> closed{false};  ///< set by the handle's destructor

    /// From begin_tick(): false when full.
    auto offer(Datagram const& datagram) noexcept -> bool
    {
        if (pending_count == PORT_BATCH) {
            return false;
        }
        pending[pending_count++] = datagram;
        return true;
    }
};

}  // namespace statusbar::aes67
