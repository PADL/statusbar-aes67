// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// libFuzzer harness for rtp::parse_rtcp and, on an SR's extension, rtp::parse_ipmx_info_block.
/// Neither may crash or read out of bounds; every view they return must lie inside the input.

#include "statusbar/rtp/rtcp_ipmx.hpp"
#include "statusbar/rtp/rtcp_packet.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace statusbar;

namespace {

void require_inside(std::span<uint8_t const> const input, void const* const data, size_t const size)
{
    auto const* const p = static_cast<uint8_t const*>(data);
    if (size != 0 && (p < input.data() || p + size > input.data() + input.size())) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const input{data, size};
    auto const compound = rtp::parse_rtcp(input);
    if (!compound) {
        return 0;
    }
    if (compound->sr) {
        require_inside(input, compound->sr->extension.data(), compound->sr->extension.size());
        require_inside(input, compound->sr->blocks.bytes.data(), compound->sr->blocks.bytes.size());
        for (size_t i = 0; i < compound->sr->blocks.count(); ++i) {
            (void)compound->sr->blocks.at(i);
        }
        if (auto const block = rtp::parse_ipmx_info_block(compound->sr->extension)) {
            require_inside(input, block->ts_refclk.data(), block->ts_refclk.size());
            require_inside(input, block->mediaclk.data(), block->mediaclk.size());
            if (block->pcm) {
                require_inside(input, block->pcm->channel_order.data(), block->pcm->channel_order.size());
            }
        }
    }
    if (compound->rr) {
        require_inside(input, compound->rr->blocks.bytes.data(), compound->rr->blocks.bytes.size());
        (void)compound->rr->blocks.find(0);
    }
    if (compound->sdes) {
        require_inside(input, compound->sdes->cname.data(), compound->sdes->cname.size());
        require_inside(input, compound->sdes->name.data(), compound->sdes->name.size());
    }
    if (compound->bye) {
        require_inside(input, compound->bye->ssrc_bytes.data(), compound->bye->ssrc_bytes.size());
        (void)compound->bye->contains(0);
        require_inside(input, compound->bye->reason.data(), compound->bye->reason.size());
    }
    return 0;
}
