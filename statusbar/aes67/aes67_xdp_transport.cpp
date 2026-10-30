// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_xdp_transport.hpp"

#include "statusbar/aes67/aes67_error.hpp"
#include "statusbar/ip/ip_ipv4.hpp"
#include "statusbar/net/net_interface_info.hpp"

#include <algorithm>
#include <cassert>

namespace statusbar::aes67 {

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
        return frame && transport_.commit(frame->first, frame->second, 0);
    }

    auto send_rendered(rtp::RtpHeader const& header, size_t const payload_length, PayloadRenderer const& render) noexcept
        -> bool override
    {
        return send_rendered_at(header, payload_length, render, 0);
    }

    auto send_rendered_at(
        rtp::RtpHeader const& header,
        size_t const payload_length,
        PayloadRenderer const& render,
        uint64_t const departure_ns) noexcept -> bool override
    {
        auto const frame = render_frame(transport_.tx_pool_, addressing_, header, payload_length, render);
        return frame && transport_.commit(frame->first, frame->second, departure_ns);
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
{  // NOLINT(statusbar-block-statements): the datagram copied into a frame, field by field
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
    delete_port_states();
    collect_retired();
    while (auto slot = tx_pool_.take()) {
        (void)context_.tx_cancel(slot->handle);
    }
}

void XdpTransport::delete_port_states() noexcept
{
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
        .tx_launch_time = options.launch_time,
    };
}

#if defined(__linux__)
namespace {

/// `queue` of `ifindex` to `mode` over a TcClient of its own.
auto settle_over_tc(int const ifindex, net::tc::LaunchTimeQueue const& queue, net::tc::LaunchTimeMode const mode)
    -> StatusValue<net::tc::LaunchTimeQueueAction>
{
    net::tc::TcClient tc;
    if (auto const opened = tc.open(); !opened) {
        return forward_failure(opened);
    }
    return net::tc::settle_launch_time_queue(tc, ifindex, queue, mode);
}

}  // namespace
#endif

auto XdpTransport::settle_launch_time(std::string_view const interface, net::tc::LaunchTimeMode const mode)
    -> StatusValue<net::tc::LaunchTimeQueueAction>
{
#if defined(__linux__)
    if (mode == net::tc::LaunchTimeMode::unsupported) {
        return net::tc::LaunchTimeQueueAction::none;
    }
    auto const ifindex = net::interface_index(interface);
    auto const queues = net::tx_queue_count(interface);
    if (!ifindex || !queues) {
        return failure(ifindex ? queues.error() : ifindex.error());
    }
    return settle_over_tc(*ifindex, launch_time_queue(*queues), mode);
#else
    (void)interface, (void)mode;
    return failure(std::errc::not_supported);
#endif
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
    std::array<std::pair<size_t, uint8_t>, 3> const bytes{{
        {VERSION_IHL, 0x45},
        {DESTINATION, group.octet(0)},
        {DESTINATION + 1, group.octet(1)},
    }};
    for (auto const& [at, value] : bytes) {
        rule.mask[at] = 0xFF;
        rule.match[at] = value;
    }
    return rule;
}

auto XdpTransport::open(std::string_view const interface, Options const& options) -> StatusValue<std::unique_ptr<XdpTransport>>
{
    auto info = find_interface(interface);
    if (!info) {
        return failure(info.error());
    }
    std::unique_ptr<XdpTransport> transport{new XdpTransport{std::move(*info)}};
    if (auto opened = transport->context_.open(
            config_for(transport->interface_.name, transport->with_launch_time(options, net::probe_tx_metadata)));
        !opened) {
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
    if (auto const checked = check_receive(destination); !checked) {
        return forward_failure(checked);
    }
    auto fd = open_group_socket(destination.address);
    if (!fd) {
        return failure(fd.error());
    }
    retain_group(destination.address);
    return std::make_unique<Port>(*this, publish_port(destination, std::move(*fd)));
}

auto XdpTransport::check_receive(Endpoint const& destination) const noexcept -> Status
{
    if (!destination.address.is_multicast()) {
        return failure(Aes67Error::unsupported_destination);
    }
    bool const new_group =
        std::ranges::find(groups_, destination.address, &std::pair<ip::IPv4Address, size_t>::first) == groups_.end();
    if (new_group && groups_.size() == MAX_GROUPS) {
        return failure(net::XdpError::too_many_filter_rules);
    }
    if (live_ports_ == MAX_PORTS || !added_.can_publish()) {
        return failure(std::errc::resource_unavailable_try_again);
    }
    return success();
}

auto XdpTransport::open_group_socket(ip::IPv4Address const& group) const -> StatusValue<net::FileDescriptor>
{
    auto fd = net::make_udp_socket();
    if (!fd) {
        return failure(fd.error());
    }
    if (auto joined = net::join_group(fd->get(), group, interface_.index); !joined) {
        return failure(joined.error());
    }
    return std::move(*fd);
}

void XdpTransport::retain_group(ip::IPv4Address const& group)
{
    auto const it = std::ranges::find(groups_, group, &std::pair<ip::IPv4Address, size_t>::first);
    if (it == groups_.end()) {
        groups_.emplace_back(group, 1);
        return;
    }
    ++it->second;
}

auto XdpTransport::publish_port(Endpoint const& destination, net::FileDescriptor fd) -> PortState*
{
    auto* const state = new PortState{destination, std::move(fd)};
    added_.publish(state);
    ++live_ports_;
    return state;
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
    return std::make_unique<XdpRtcpPort>(*this, publish_rtcp_pipe(options.destination), std::move(*socket));
}

auto XdpTransport::publish_rtcp_pipe(Endpoint const& destination) -> RtcpPipe*
{
    auto* const pipe = new RtcpPipe{destination};
    rtcp_added_.publish(pipe);
    ++live_rtcp_;
    return pipe;
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
        if (std::ranges::contains(std::span{groups}.first(count), group)) {
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
    if (update_ports()) {
        install_rules();
    }
    update_rtcp_pipes();
    start_cycle();
    receive_frames();
}

auto XdpTransport::update_ports() noexcept -> bool
{
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
    return changed;
}

auto XdpTransport::commit(net::EthernetTxSlot const& slot, size_t const length, uint64_t const departure_ns) noexcept -> bool
{
    // A pooled frame was taken untimed (tx_start): on a socket with launch time every frame gets its
    // time now, since the driver may otherwise reuse the previous frame's.
    if (context_.tx_launch_time()) {
        context_.tx_set_launch_time(slot.handle, static_cast<int64_t>(launch_clock_.next(departure_ns, net::tai_now_ns)));
    }
    return context_.tx_commit(slot.handle, length).has_value();  // a full ring frees the frame
}

void XdpTransport::start_cycle() noexcept
{
    // Reap TX completions, hold back the senders' frames, then offer the kernel the rest.
    (void)context_.tx_complete();
    refill_tx_pool();
    (void)context_.fill_replenish();
    context_.begin_cycle();
}

void XdpTransport::receive_frames() noexcept
{
    auto const ports = std::span<PortState* const>{ports_}.first(port_count_);
    auto const pipes = std::span<RtcpPipe* const>{rtcp_pipes_}.first(rtcp_count_);
    while (held_count_ < RECEIVE_BATCH) {
        auto slot = context_.rx_start();
        if (!slot || !*slot) {
            break;
        }
        auto const& rx = **slot;
        if (dispatch_frame(rx.buffer, ports, pipes, refusals_)) {
            held_[held_count_++] = rx.handle;
        } else {
            (void)context_.rx_release(rx.handle);
        }
    }
}

namespace {

/// What offer_to_ports() did with a datagram.
enum class Offered : uint8_t
{
    kept,       ///< a port kept it
    unmatched,  ///< no port has its destination
    refused,    ///< ports have it, and none kept it
};

/// What became of a datagram for `matched` ports, `kept` by one or not: a refusal counted once per frame,
/// none for the stream's own RTCP on its RTP port (RFC 5761).
auto outcome(
    bool const matched, bool const kept, std::span<uint8_t const> const payload, bool const rtp, RefusalCounter& refusals) noexcept
    -> Offered
{
    if (!matched || kept) {
        return matched ? Offered::kept : Offered::unmatched;
    }
    if (!is_multiplexed_rtcp(payload)) {
        refusals.count(rtp ? Refusal::port_full : Refusal::not_rtp);
    }
    return Offered::refused;
}

/// Offers an RTP datagram to the receive ports of `destination`; several receivers of one stream share it.
auto offer_to_ports(
    std::span<XdpTransport::PortState* const> const ports,
    Endpoint const& destination,
    Datagram const& datagram,
    RefusalCounter& refusals) noexcept -> Offered
{
    bool const rtp = !is_multiplexed_rtcp(datagram.payload) && rtp::parse_rtp(datagram.payload).has_value();
    bool matched = false;
    bool kept = false;
    for (auto* const port : ports) {
        if (port->destination == destination && !port->closed.load(std::memory_order_relaxed)) {
            matched = true;
            kept = (rtp && port->offer(datagram)) || kept;
        }
    }
    return outcome(matched, kept, datagram.payload, rtp, refusals);
}

/// Copies a datagram to the RTCP pipes of `destination`; true when one has it.
auto offer_to_rtcp(
    std::span<XdpTransport::RtcpPipe* const> const pipes, Endpoint const& destination, Datagram const& datagram) noexcept -> bool
{
    bool matched = false;
    for (auto* const pipe : pipes) {
        if (pipe->destination() == destination && !pipe->closed.load(std::memory_order_relaxed)) {
            (void)pipe->offer(datagram);  // copied: the frame need not be held
            matched = true;
        }
    }
    return matched;
}

/// A frame's destination, and its datagram with its source.
auto addressed(rtp::UdpFrame const& frame) noexcept -> std::pair<Endpoint, Datagram>
{
    auto const& a = frame.addressing;
    return {
        Endpoint{.address = a.destination_address, .port = a.destination_port},
        Datagram{.payload = frame.datagram, .source = {.address = a.source_address, .port = a.source_port}},
    };
}

}  // namespace

auto XdpTransport::dispatch_frame(
    std::span<uint8_t const> const frame,
    std::span<PortState* const> const ports,
    std::span<RtcpPipe* const> const pipes,
    RefusalCounter& refusals) noexcept -> bool
{
    auto const udp = rtp::parse_udp_frame(frame);
    if (!udp) {
        refusals.count(Refusal::malformed);
        return false;
    }
    auto const [destination, datagram] = addressed(*udp);
    auto const offered = offer_to_ports(ports, destination, datagram, refusals);
    if (!offer_to_rtcp(pipes, destination, datagram) && offered == Offered::unmatched) {
        refusals.count(Refusal::unmatched);
    }
    return offered == Offered::kept;
}

void XdpTransport::end_tick() noexcept
{
    bool const ok = context_.tx_flush().has_value();
    tx_health_.record(ok, ok ? 0 : context_.stats().tx_flush_errno);
    socket_stats_.store(context_.stats());
}

}  // namespace statusbar::aes67
