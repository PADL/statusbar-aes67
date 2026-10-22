// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

// FrameRing: one input stream's audio, in its own wire format, addressed by bus
// sample index.
//
// The bridge copies audio exactly twice: once into a ring as a packet arrives,
// once out of it as an outgoing packet is built (or not at all, when the sender
// transmits straight from view()). A ring stores whole frames as opaque bytes
// -- AES67 L24 and AAF INT_24BIT are the same bytes -- so neither copy looks at
// a sample, and reframing (48-frame RTP packets in, 6-frame AAF packets out) is
// just reading a different-sized slice.
//
// Validity. Frames are grouped into blocks of one bus tick (block_frames, at
// most 32). Each block carries one atomic word: the low 32 bits of the block
// number it holds and a mask of the frames written. A reader sees a frame only
// when its block holds the requested block number and the frame's bit is set;
// anything else -- never written, lost, late, a stale lap -- reads as silence.
// A frame is written at most once per lap: duplicates (retransmissions, a
// redundant path) are skipped, so a reader never sees bytes change under it.
//
// Race freedom without a seqlock. Writer and readers are kept a lap apart by a
// window around the bus's current index ("now"): writes are accepted only in
// [now - max_behind, now + max_ahead], and reads only in the same window. With
// the ring longer than the window plus a block either side, the writer cannot
// begin a new lap of a slot while any reader may still read the old one, so
// every byte a reader copies was published by a release store it acquired.
// Callers pass their own idea of now; the slack covers threads disagreeing by
// up to one block.
//
// Threading: one writer thread; any number of reader threads.

#pragma once

#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace statusbar::mediabus {

class FrameRing
{
  public:
    struct Config
    {
        std::uint32_t frame_bytes{0};         // channels * bytes per sample
        std::uint32_t block_frames{6};        // frames per bus tick, 1..32
        std::uint32_t blocks{256};            // ring length in blocks
        std::uint32_t max_ahead_frames{960};  // furthest a write or read may be ahead of now
        std::uint32_t max_behind_frames{96};  // furthest a write or read may be behind now
    };

    /// Whether the config describes a usable ring: nonzero sizes, block_frames at
    /// most 32, and the ring longer than the window plus a block either side.
    [[nodiscard]] static auto validate(Config const& config) noexcept -> Status;

    /// config must pass validate().
    explicit FrameRing(Config const& config, std::pmr::memory_resource* mr = std::pmr::get_default_resource());

    FrameRing(FrameRing const&) = delete;
    auto operator=(FrameRing const&) -> FrameRing& = delete;

    [[nodiscard]] auto config() const noexcept -> Config const& { return config_; }
    [[nodiscard]] auto frames() const noexcept -> std::uint64_t { return frames_; }

    /// Writer. Store `bytes` (whole frames) as frames first_index onwards. Frames
    /// outside the window and frames already written this lap are skipped. Returns
    /// the number of frames stored.
    auto write(std::uint64_t now_index, std::uint64_t first_index, std::span<std::byte const> bytes) noexcept
        -> std::size_t;

    /// Reader. Copy frames [first_index, first_index + out.size() / frame_bytes)
    /// into `out`, zero-filling every frame that is not readable. Returns the
    /// number of frames that carried audio.
    auto read(std::uint64_t now_index, std::uint64_t first_index, std::span<std::byte> out) const noexcept
        -> std::size_t;

    /// Reader, zero copy. The bytes of frames [first_index, first_index + count)
    /// as at most two spans (two when the range wraps), or nothing unless every
    /// frame is readable. The spans stay valid while the range is in the window.
    struct View
    {
        std::array<std::span<std::byte const>, 2> parts{};
        bool complete{false};
    };
    [[nodiscard]] auto view(std::uint64_t now_index, std::uint64_t first_index, std::uint32_t count) const noexcept
        -> View;

  private:
    [[nodiscard]] auto in_window(std::uint64_t now_index, std::uint64_t index) const noexcept -> bool;
    [[nodiscard]] auto frame_offset(std::uint64_t index) const noexcept -> std::size_t;
    [[nodiscard]] auto readable_mask(std::uint64_t block) const noexcept -> std::uint32_t;

    Config config_;
    std::uint64_t frames_;
    std::pmr::vector<std::byte> storage_;
    // Per block: (block number & 0xffffffff) << 32 | written-frame mask.
    std::pmr::vector<std::atomic<std::uint64_t>> state_;
};

}  // namespace statusbar::mediabus
