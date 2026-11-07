// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_split_transport.hpp"

namespace statusbar::aes67 {

auto SplitTransport::open_send(SendOptions const& options) -> StatusValue<std::unique_ptr<SendPort>>
{
    if (options.destination.address.is_multicast()) {
        return multicast_->open_send(options);
    }
    SendOptions untagged = options;
    untagged.vlan.reset();  // a reservation is to a group: a unicast stream is never tagged
    return unicast_->open_send(untagged);
}

void SplitTransport::begin_tick() noexcept
{
    multicast_->begin_tick();
    unicast_->begin_tick();
}

void SplitTransport::end_tick() noexcept
{
    multicast_->end_tick();
    unicast_->end_tick();
}

auto SplitTransport::send_error() const noexcept -> std::error_code
{
    auto const error = multicast_->send_error();
    return error ? error : unicast_->send_error();
}

auto SplitTransport::receive_refusals() const noexcept -> ReceiveRefusals
{
    auto const a = multicast_->receive_refusals();
    auto const b = unicast_->receive_refusals();
    return {
        .malformed = a.malformed + b.malformed,
        .unmatched = a.unmatched + b.unmatched,
        .not_rtp = a.not_rtp + b.not_rtp,
        .port_full = a.port_full + b.port_full,
    };
}

void SplitTransport::reset_counters() noexcept
{
    multicast_->reset_counters();
    unicast_->reset_counters();
}

}  // namespace statusbar::aes67
