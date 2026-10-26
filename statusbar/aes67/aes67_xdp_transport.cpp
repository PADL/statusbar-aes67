// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_xdp_transport.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/ip/ip_ipv4.hpp"

#include <algorithm>
#include <cassert>

namespace statusbar::aes67 {

/// A receive port's state: made on the control thread, used on the real-time
/// thread from the tick that takes it until the tick that retires it, freed on
/// the control thread.
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

/// The handle a receiver owns. receive() runs on the real-time thread; the
/// destructor on the control thread, once the receiver is no longer drained.
class XdpTransport::Port final : public ReceivePort
{
  public:
    Port(XdpTransport& transport, PortState* state) noexcept
        : transport_{transport}
        , state_{state}
    {}

    ~Port() override { transport_.port_closed(state_); }
    Port(Port const&) = delete;
    auto operator=(Port const&) -> Port& = delete;
    Port(Port&&) = delete;
    auto operator=(Port&&) -> Port& = delete;

    auto receive(std::span<Datagram> const out) noexcept -> size_t override
    {
        auto& s = *state_;
        size_t const n = std::min(out.size(), s.pending_count);
        std::copy_n(s.pending.begin(), n, out.begin());
        // What did not fit stays for the next call (its frame stays held until
        // the next begin_tick(), so a caller must drain within the tick).
        std::copy(
            s.pending.begin() + static_cast<std::ptrdiff_t>(n),
            s.pending.begin() + static_cast<std::ptrdiff_t>(s.pending_count),
            s.pending.begin());
        s.pending_count -= n;
        return n;
    }

  private:
    XdpTransport& transport_;
    PortState* state_;
};

class XdpTransport::Sender final : public SendPort
{
  public:
    Sender(XdpTransport& transport, rtp::FrameAddressing const& addressing) noexcept
        : transport_{transport}
        , addressing_{addressing}
    {}

    auto send(rtp::RtpHeader const& header, std::span<uint8_t const> const payload) noexcept -> bool override
    {
        auto const frame = write_frame(transport_.tx_pool_, addressing_, header, payload);
        if (!frame) {
            return false;
        }
        return transport_.context_.tx_commit(frame->first.handle, frame->second).has_value();  // a full ring frees the frame
    }

    auto send_rendered(rtp::RtpHeader const& header, size_t const payload_length, PayloadRenderer const& render) noexcept
        -> bool override
    {
        auto const frame = render_frame(transport_.tx_pool_, addressing_, header, payload_length, render);
        if (!frame) {
            return false;
        }
        return transport_.context_.tx_commit(frame->first.handle, frame->second).has_value();  // a full ring frees the frame
    }

    /// The transport flushes every sender's frames in end_tick().
    auto flush() noexcept -> size_t override { return 0; }

  private:
    XdpTransport& transport_;
    rtp::FrameAddressing addressing_;
};

/// An RTCP port: the kernel socket sends, and receives what the program passes; the pipe
/// receives what it redirects. Reactor thread; the destructor on the control thread.
class XdpRtcpPort final : public RtcpPort
{
  public:
    /// Datagrams one receive() takes from the pipe at most.
    static constexpr size_t BATCH = 8;

    XdpRtcpPort(XdpTransport& transport, XdpTransport::RtcpPipe* pipe, std::unique_ptr<RtcpPort> socket) noexcept
        : transport_{transport}
        , pipe_{pipe}
        , socket_{std::move(socket)}
    {}

    ~XdpRtcpPort() override { transport_.rtcp_closed(pipe_); }
    XdpRtcpPort(XdpRtcpPort const&) = delete;
    auto operator=(XdpRtcpPort const&) -> XdpRtcpPort& = delete;
    XdpRtcpPort(XdpRtcpPort&&) = delete;
    auto operator=(XdpRtcpPort&&) -> XdpRtcpPort& = delete;

    /// Redirected datagrams wake no descriptor: the session polls.
    [[nodiscard]] auto fd() const noexcept -> int override { return -1; }

    auto receive(std::span<Datagram> const out) noexcept -> size_t override
    {
        size_t const n = pipe_->take(out.first(std::min(out.size(), BATCH)), frames_);
        return n + socket_->receive(out.subspan(n));
    }

    auto send(std::span<uint8_t const> const packet) noexcept -> Status override { return socket_->send(packet); }

    [[nodiscard]] auto dropped() const noexcept -> uint64_t override { return pipe_->dropped() + socket_->dropped(); }

  private:
    XdpTransport& transport_;
    XdpTransport::RtcpPipe* pipe_;
    std::unique_ptr<RtcpPort> socket_;
    std::array<XdpTransport::RtcpPipe::Frame, BATCH> frames_{};
};

auto XdpTransport::RtcpPipe::offer(Datagram const& datagram) noexcept -> bool
{
    if (datagram.payload.size() > MAX_DATAGRAM || !queue_.can_publish()) {
        dropped_.store(dropped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return false;
    }
    Frame frame;  // NOLINT(cppcoreguidelines-pro-type-member-init): only `length` bytes are copied
    std::ranges::copy(datagram.payload, frame.bytes.begin());
    frame.length = static_cast<uint16_t>(datagram.payload.size());
    frame.source = datagram.source;
    queue_.publish(frame);
    return true;
}

auto XdpTransport::RtcpPipe::take(std::span<Datagram> const out, std::span<Frame> const storage) noexcept -> size_t
{
    size_t const want = std::min(out.size(), storage.size());
    size_t n = 0;
    while (n < want) {
        auto frame = queue_.try_consume();
        if (!frame) {
            break;
        }
        storage[n] = *frame;
        out[n] = {.payload = std::span<uint8_t const>{storage[n].bytes}.first(storage[n].length), .source = storage[n].source};
        ++n;
    }
    return n;
}

auto XdpTransport::write_frame(
    TxPool& pool,
    rtp::FrameAddressing const& addressing,
    rtp::RtpHeader const& header,
    std::span<uint8_t const> const payload) noexcept -> std::optional<std::pair<net::EthernetTxSlot, size_t>>
{
    return render_frame(pool, addressing, header, payload.size(), [payload](std::span<uint8_t> const out) noexcept {
        std::ranges::copy(payload, out.begin());
        return true;
    });
}

auto XdpTransport::render_frame(
    TxPool& pool,
    rtp::FrameAddressing const& addressing,
    rtp::RtpHeader const& header,
    size_t const payload_length,
    PayloadRenderer const& render) noexcept -> std::optional<std::pair<net::EthernetTxSlot, size_t>>
{
    auto slot = pool.take();
    if (!slot) {
        return std::nullopt;
    }
    auto const offset = rtp::serialize_rtp_frame_headers(addressing, header, payload_length, slot->buffer);
    if (!offset || !render(slot->buffer.subspan(*offset, payload_length))) {
        (void)pool.put(*slot);  // back to the pool unused
        return std::nullopt;
    }
    return std::pair{*slot, *offset + payload_length};
}

XdpTransport::XdpTransport(InterfaceInfo interface) noexcept
    : interface_{std::move(interface)}
{}

XdpTransport::~XdpTransport()
{
    assert(live_ports_ == 0 && "every receive port must be destroyed before its transport");
    assert(live_rtcp_ == 0 && "every RTCP port must be destroyed before its transport");
    release_held();
    for (size_t i = 0; i < port_count_; ++i) {
        delete ports_[i];
    }
    while (auto state = added_.try_consume()) {
        delete *state;
    }
    for (size_t i = 0; i < rtcp_count_; ++i) {
        delete rtcp_pipes_[i];
    }
    while (auto pipe = rtcp_added_.try_consume()) {
        delete *pipe;
    }
    collect_retired();
    while (auto slot = tx_pool_.take()) {
        (void)context_.tx_cancel(slot->handle);
    }
}

auto XdpTransport::config_for(std::string_view const interface, Options const& options) noexcept -> net::XdpConfig
{
    return net::XdpConfig{
        .interface_name = interface,
        .filter_rules = {},
        .default_action = net::XdpAction::pass_kernel,
        .queue_id = QUEUE,
        .fill_ring_size = FILL_RING,
        .umem_frame_count = UMEM_FRAMES,
        .rx_drain_max = RECEIVE_BATCH,
        .bind_mode = options.bind_mode,
        .attach_mode = options.attach_mode,
        .pin_path = options.pin_path,
    };
}

auto XdpTransport::group_rule(ip::IPv4Address const& group) noexcept -> net::CompiledRule<32>
{
    auto rule = net::ClassifierRuleBuilder<32>{}
                    .dest_mac(rtp::multicast_mac(group))
                    .ethertype(0x0800)
                    .ipv4_protocol(ip::IPv4Header::PROTOCOL_UDP)
                    .vlan_untagged()
                    .result(net::flag::process)
                    .build()[0];
    // Past what the builder writes: IHL 5 (so the offsets below hold), and the
    // destination address's first two octets, the bytes of the 32-byte window
    // the MAC does not already pin (it carries the low 23 bits).
    constexpr size_t VERSION_IHL = 14;
    constexpr size_t DESTINATION = 14 + 16;
    rule.mask[VERSION_IHL] = 0xFF;
    rule.match[VERSION_IHL] = 0x45;
    rule.mask[DESTINATION] = 0xFF;
    rule.match[DESTINATION] = group.octet(0);
    rule.mask[DESTINATION + 1] = 0xFF;
    rule.match[DESTINATION + 1] = group.octet(1);
    return rule;
}

auto XdpTransport::open(std::string_view const interface, Options const& options) -> StatusValue<std::unique_ptr<XdpTransport>>
{
    auto info = find_interface(interface);
    if (!info) {
        return failure(info.error());
    }
    std::unique_ptr<XdpTransport> transport{new XdpTransport{std::move(*info)}};
    if (auto opened = transport->context_.open(config_for(transport->interface_.name, options)); !opened) {
        return failure(opened.error());
    }
    if (options.steer_rss) {
        transport->rss_status_ =
            net::set_rss_indirection_all(transport->interface_.name, QUEUE).transform([](net::RssIndirection const&) {});
    }
    return transport;
}

auto XdpTransport::rules_status() const noexcept -> Status
{
    auto const error = rules_error_.load();
    if (error) {
        return failure(error);
    }
    return success();
}

auto XdpTransport::open_receive(Endpoint const& destination) -> StatusValue<std::unique_ptr<ReceivePort>>
{
    collect_retired();
    if (!destination.address.is_multicast()) {
        return failure(Aes67Error::unsupported_destination);
    }
    auto group = std::ranges::find(groups_, destination.address, &std::pair<ip::IPv4Address, size_t>::first);
    if (group == groups_.end() && groups_.size() == MAX_GROUPS) {
        return failure(net::XdpError::too_many_filter_rules);
    }
    if (live_ports_ == MAX_PORTS || !added_.can_publish()) {
        return failure(std::errc::resource_unavailable_try_again);
    }
    auto fd = net::make_udp_socket();
    if (!fd) {
        return failure(fd.error());
    }
    if (auto joined = net::join_group(fd->get(), destination.address, interface_.index); !joined) {
        return failure(joined.error());
    }
    if (group == groups_.end()) {
        groups_.emplace_back(destination.address, 0);
        group = std::prev(groups_.end());
    }
    ++group->second;
    ++live_ports_;
    auto* const state = new PortState{destination, std::move(*fd)};
    added_.publish(state);
    return std::make_unique<Port>(*this, state);
}

void XdpTransport::port_closed(PortState* const state) noexcept
{
    state->closed.store(true, std::memory_order_release);
    auto const group = std::ranges::find(groups_, state->destination.address, &std::pair<ip::IPv4Address, size_t>::first);
    if (group != groups_.end() && --group->second == 0) {
        groups_.erase(group);  // no allocation
    }
    --live_ports_;
    collect_retired();
}

void XdpTransport::collect_retired() noexcept
{
    while (auto state = retired_.try_consume()) {
        delete *state;
    }
    while (auto pipe = rtcp_retired_.try_consume()) {
        delete *pipe;
    }
}

auto XdpTransport::open_rtcp(RtcpOptions const& options) -> StatusValue<std::unique_ptr<RtcpPort>>
{
    collect_retired();
    if (!options.destination.address.is_multicast()) {
        return failure(Aes67Error::unsupported_destination);
    }
    if (live_rtcp_ == MAX_RTCP_PORTS || !rtcp_added_.can_publish()) {
        return failure(std::errc::resource_unavailable_try_again);
    }
    auto socket = open_rtcp_socket(interface_, options);
    if (!socket) {
        return failure(socket.error());
    }
    auto* const pipe = new RtcpPipe{options.destination};
    rtcp_added_.publish(pipe);
    ++live_rtcp_;
    return std::make_unique<XdpRtcpPort>(*this, pipe, std::move(*socket));
}

void XdpTransport::rtcp_closed(RtcpPipe* const pipe) noexcept
{
    pipe->closed.store(true, std::memory_order_release);
    --live_rtcp_;
    collect_retired();
}

void XdpTransport::update_rtcp_pipes() noexcept
{
    while (rtcp_count_ < MAX_RTCP_PORTS) {
        auto pipe = rtcp_added_.try_consume();
        if (!pipe) {
            break;
        }
        rtcp_pipes_[rtcp_count_++] = *pipe;
    }
    for (size_t i = 0; i < rtcp_count_;) {
        if (rtcp_pipes_[i]->closed.load(std::memory_order_acquire) && rtcp_retired_.try_publish(rtcp_pipes_[i])) {
            rtcp_pipes_[i] = rtcp_pipes_[--rtcp_count_];
        } else {
            ++i;
        }
    }
}

auto XdpTransport::open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>>
{
    collect_retired();
    if (!options.destination.address.is_multicast()) {
        return failure(Aes67Error::unsupported_destination);
    }
    rtp::FrameAddressing const addressing{
        .destination_mac = rtp::multicast_mac(options.destination.address),
        .source_mac = context_.hardware_address(),
        .vlan = std::nullopt,
        .source_address = interface_.address,
        .destination_address = options.destination.address,
        .source_port = options.destination.port,
        .destination_port = options.destination.port,
        .ttl = options.ttl,
        .dscp = options.dscp,
    };
    return std::make_unique<Sender>(*this, addressing);
}

void XdpTransport::install_rules() noexcept
{
    std::array<ip::IPv4Address, MAX_GROUPS> groups{};
    size_t count = 0;
    std::error_code error;
    for (size_t i = 0; i < port_count_; ++i) {
        auto const& group = ports_[i]->destination.address;
        auto* const end = groups.begin() + static_cast<std::ptrdiff_t>(count);
        if (std::find(groups.begin(), end, group) != end) {
            continue;
        }
        if (count == MAX_GROUPS) {
            error = make_error_code(net::XdpError::too_many_filter_rules);
            break;
        }
        groups[count] = group;
        rules_[count] = group_rule(group);
        ++count;
    }
    if (auto installed = context_.set_rules(std::span{rules_}.first(count)); !installed && !error) {
        error = installed.error();
    }
    rules_error_.store(error);
}

void XdpTransport::release_held() noexcept
{
    for (size_t i = 0; i < held_count_; ++i) {
        (void)context_.rx_release(held_[i]);
    }
    held_count_ = 0;
}

void XdpTransport::refill_tx_pool() noexcept
{
    while (tx_pool_.size() < TX_POOL) {
        auto slot = context_.tx_start();
        if (!slot) {
            break;
        }
        (void)tx_pool_.put(*slot);
    }
}

void XdpTransport::begin_tick() noexcept
{
    release_held();

    // Ports come and go here only.
    bool changed = false;
    while (port_count_ < MAX_PORTS) {
        auto state = added_.try_consume();
        if (!state) {
            break;
        }
        ports_[port_count_++] = *state;
        changed = true;
    }
    for (size_t i = 0; i < port_count_;) {
        if (ports_[i]->closed.load(std::memory_order_acquire) && retired_.try_publish(ports_[i])) {
            ports_[i] = ports_[--port_count_];
            changed = true;
        } else {
            ports_[i]->pending_count = 0;
            ++i;
        }
    }
    if (changed) {
        install_rules();
    }
    update_rtcp_pipes();

    // Reap TX completions, hold back the senders' frames, then offer the
    // kernel the rest.
    (void)context_.tx_complete();
    refill_tx_pool();
    (void)context_.fill_replenish();
    context_.begin_cycle();
    while (held_count_ < RECEIVE_BATCH) {
        auto slot = context_.rx_start();
        if (!slot || !*slot) {
            break;
        }
        auto const& rx = **slot;
        bool kept = false;
        if (auto const frame = rtp::parse_udp_frame(rx.buffer)) {
            Endpoint const destination{
                .address = frame->addressing.destination_address, .port = frame->addressing.destination_port};
            Datagram const datagram{
                .payload = frame->datagram,
                .source = {.address = frame->addressing.source_address, .port = frame->addressing.source_port}};
            std::optional<bool> rtp;  // parsed only for a receive port's destination
            for (size_t i = 0; i < port_count_; ++i) {
                auto& port = *ports_[i];
                if (port.destination != destination || port.closed.load(std::memory_order_relaxed)) {
                    continue;
                }
                if (!rtp) {
                    rtp = rtp::parse_rtp(frame->datagram).has_value();
                }
                if (*rtp && port.offer(datagram)) {
                    kept = true;  // several receivers of one stream share the frame
                }
            }
            for (size_t i = 0; i < rtcp_count_; ++i) {
                auto& pipe = *rtcp_pipes_[i];
                if (pipe.destination() == destination && !pipe.closed.load(std::memory_order_relaxed)) {
                    (void)pipe.offer(datagram);  // copied: the frame need not be held
                }
            }
        }
        if (kept) {
            held_[held_count_++] = rx.handle;
        } else {
            (void)context_.rx_release(rx.handle);
        }
    }
}

void XdpTransport::end_tick() noexcept
{
    bool const ok = context_.tx_flush().has_value();
    tx_health_.record(ok, ok ? 0 : context_.stats().tx_flush_errno);
    socket_stats_.store(context_.stats());
}

}  // namespace statusbar::aes67
