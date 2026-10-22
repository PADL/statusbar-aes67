// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/mediabus/mediabus_frame_ring.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace statusbar::mediabus {

namespace {

constexpr std::uint32_t MAX_BLOCK_FRAMES = 32;

[[nodiscard]] constexpr auto block_tag(std::uint64_t block) noexcept -> std::uint64_t
{
    return block & 0xffff'ffffU;
}

// Frames [offset, offset + count) of a block as a mask.
[[nodiscard]] constexpr auto run_mask(std::uint32_t offset, std::uint32_t count) noexcept -> std::uint32_t
{
    return static_cast<std::uint32_t>(((std::uint64_t{1} << count) - 1) << offset);
}

}  // namespace

auto FrameRing::validate(Config const& config) noexcept -> Status
{
    if (config.frame_bytes == 0 || config.block_frames == 0 || config.block_frames > MAX_BLOCK_FRAMES ||
        config.blocks == 0) {
        return failure(std::errc::invalid_argument);
    }
    // A writer at now + ahead and a reader at now - behind, with their nows a block
    // apart, must never touch the same slot in different laps.
    std::uint64_t const frames = std::uint64_t{config.blocks} * config.block_frames;
    std::uint64_t const window =
        std::uint64_t{config.max_ahead_frames} + config.max_behind_frames + (2 * std::uint64_t{config.block_frames});
    if (frames <= window) {
        return failure(std::errc::invalid_argument);
    }
    return success();
}

FrameRing::FrameRing(Config const& config, std::pmr::memory_resource* mr)
    : config_{config}
    , frames_{std::uint64_t{config.blocks} * config.block_frames}
    , storage_(frames_ * config.frame_bytes, std::byte{0}, mr)
    , state_(config.blocks, mr)
{}

auto FrameRing::in_window(std::uint64_t now_index, std::uint64_t index) const noexcept -> bool
{
    std::uint64_t const lo = now_index - std::min<std::uint64_t>(now_index, config_.max_behind_frames);
    return index >= lo && index <= now_index + config_.max_ahead_frames;
}

auto FrameRing::frame_offset(std::uint64_t index) const noexcept -> std::size_t
{
    return static_cast<std::size_t>(index % frames_) * config_.frame_bytes;
}

auto FrameRing::readable_mask(std::uint64_t block) const noexcept -> std::uint32_t
{
    std::uint64_t const state = state_[block % config_.blocks].load(std::memory_order_acquire);
    if ((state >> 32U) != block_tag(block)) {
        return 0;
    }
    return static_cast<std::uint32_t>(state);
}

auto FrameRing::write(std::uint64_t now_index, std::uint64_t first_index, std::span<std::byte const> bytes) noexcept
    -> std::size_t
{
    std::uint32_t const fb = config_.frame_bytes;
    std::uint32_t const bf = config_.block_frames;
    std::uint64_t const count = bytes.size() / fb;
    std::size_t stored = 0;

    std::uint64_t i = 0;
    while (i < count) {
        std::uint64_t const index = first_index + i;
        std::uint64_t const block = index / bf;
        auto const offset = static_cast<std::uint32_t>(index % bf);
        auto const run = static_cast<std::uint32_t>(std::min<std::uint64_t>(bf - offset, count - i));
        // The window bounds are not block-aligned, so test each end of the run.
        if (!in_window(now_index, index) || !in_window(now_index, index + run - 1)) {
            // Rare (startup, a late or early packet): fall back to one frame at a time.
            if (in_window(now_index, index)) {
                stored += write(now_index, index, bytes.subspan(i * fb, fb));
            }
            ++i;
            continue;
        }

        auto& slot = state_[block % config_.blocks];
        // Only this thread stores to the slot, so a relaxed load sees its own last store.
        std::uint64_t state = slot.load(std::memory_order_relaxed);
        if ((state >> 32U) != block_tag(block)) {
            state = block_tag(block) << 32U;  // a new lap: nothing written yet
        }
        auto const written = static_cast<std::uint32_t>(state);
        std::uint32_t const run_bits = run_mask(offset, run);
        std::uint32_t const fresh = run_bits & ~written;

        std::byte const* const src = bytes.data() + (i * fb);
        std::byte* const dst = storage_.data() + frame_offset(index);
        if (fresh == run_bits) {
            std::memcpy(dst, src, std::size_t{run} * fb);
        } else {
            for (std::uint32_t f = 0; f < run; ++f) {
                if ((fresh & (1U << (offset + f))) != 0) {
                    std::memcpy(dst + (std::size_t{f} * fb), src + (std::size_t{f} * fb), fb);
                }
            }
        }
        if (fresh != 0) {
            slot.store(state | fresh, std::memory_order_release);
            stored += static_cast<std::size_t>(std::popcount(fresh));
        }
        i += run;
    }
    return stored;
}

auto FrameRing::read(std::uint64_t now_index, std::uint64_t first_index, std::span<std::byte> out) const noexcept
    -> std::size_t
{
    std::uint32_t const fb = config_.frame_bytes;
    std::uint32_t const bf = config_.block_frames;
    std::uint64_t const count = out.size() / fb;
    std::memset(out.data(), 0, count * fb);
    std::size_t real = 0;

    std::uint64_t i = 0;
    while (i < count) {
        std::uint64_t const index = first_index + i;
        std::uint64_t const block = index / bf;
        auto const offset = static_cast<std::uint32_t>(index % bf);
        auto const run = static_cast<std::uint32_t>(std::min<std::uint64_t>(bf - offset, count - i));
        if (!in_window(now_index, index) || !in_window(now_index, index + run - 1)) {
            if (in_window(now_index, index)) {
                real += read(now_index, index, out.subspan(i * fb, fb));
            }
            ++i;
            continue;
        }

        std::uint32_t const run_bits = run_mask(offset, run);
        std::uint32_t const present = readable_mask(block) & run_bits;
        std::byte const* const src = storage_.data() + frame_offset(index);
        std::byte* const dst = out.data() + (i * fb);
        if (present == run_bits) {
            std::memcpy(dst, src, std::size_t{run} * fb);
        } else if (present != 0) {
            for (std::uint32_t f = 0; f < run; ++f) {
                if ((present & (1U << (offset + f))) != 0) {
                    std::memcpy(dst + (std::size_t{f} * fb), src + (std::size_t{f} * fb), fb);
                }
            }
        }
        real += static_cast<std::size_t>(std::popcount(present));
        i += run;
    }
    return real;
}

auto FrameRing::view(std::uint64_t now_index, std::uint64_t first_index, std::uint32_t count) const noexcept -> View
{
    if (count == 0 || count > frames_ || !in_window(now_index, first_index) ||
        !in_window(now_index, first_index + count - 1)) {
        return {};
    }
    std::uint32_t const bf = config_.block_frames;
    std::uint64_t index = first_index;
    std::uint64_t const end = first_index + count;
    while (index < end) {
        auto const offset = static_cast<std::uint32_t>(index % bf);
        auto const run = static_cast<std::uint32_t>(std::min<std::uint64_t>(bf - offset, end - index));
        std::uint32_t const run_bits = run_mask(offset, run);
        if ((readable_mask(index / bf) & run_bits) != run_bits) {
            return {};
        }
        index += run;
    }

    std::uint32_t const fb = config_.frame_bytes;
    std::uint64_t const first_slot = first_index % frames_;
    std::uint64_t const head = std::min<std::uint64_t>(count, frames_ - first_slot);
    View result{.complete = true};
    result.parts[0] = std::span<std::byte const>(storage_).subspan(first_slot * fb, head * fb);
    if (head < count) {
        result.parts[1] = std::span<std::byte const>(storage_).first((count - head) * fb);
    }
    return result;
}

}  // namespace statusbar::mediabus
