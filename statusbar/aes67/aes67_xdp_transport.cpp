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

    /// The transport flushes every sender's frames in end_tick().
    auto flush() noexcept -> size_t override { return 0; }

  private:
    XdpTransport& transport_;
    rtp::FrameAddressing addressing_;
};

auto XdpTransport::write_frame(
    TxPool& pool,
    rtp::FrameAddressing const& addressing,
    rtp::RtpHeader const& header,
    std::span<uint8_t const> const payload) noexcept -> std::optional<std::pair<net::EthernetTxSlot, size_t>>
{
    auto slot = pool.take();
    if (!slot) {
        return std::nullopt;
    }
    auto const length = rtp::serialize_rtp_frame(addressing, header, payload, slot->buffer);
    if (!length) {
        (void)pool.put(*slot);  // back to the pool unused
        return std::nullopt;
    }
    return std::pair{*slot, *length};
}

XdpTransport::XdpTransport(InterfaceInfo interface) noexcept
    : interface_{std::move(interface)}
{}

XdpTransport::~XdpTransport()
{
    assert(live_ports_ == 0 && "every receive port must be destroyed before its transport");
    release_held();
    for (size_t i = 0; i < port_count_; ++i) {
        delete ports_[i];
    }
    while (auto state = added_.try_consume()) {
        delete *state;
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
        .queue_id = 0,
        .fill_ring_size = FILL_RING,
        .umem_frame_count = UMEM_FRAMES,
        .rx_drain_max = RECEIVE_BATCH,
        .bind_mode = options.bind_mode,
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
        auto const end = groups.begin() + static_cast<std::ptrdiff_t>(count);
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
        if (auto const frame = rtp::parse_rtp_frame(rx.buffer)) {
            Endpoint const destination{frame->addressing.destination_address, frame->addressing.destination_port};
            Datagram const datagram{
                .payload = frame->datagram, .source = {frame->addressing.source_address, frame->addressing.source_port}};
            for (size_t i = 0; i < port_count_; ++i) {
                auto& port = *ports_[i];
                if (port.destination == destination && !port.closed.load(std::memory_order_relaxed) && port.offer(datagram)) {
                    kept = true;  // several receivers of one stream share the frame
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
    (void)context_.tx_flush();
}

}  // namespace statusbar::aes67
