// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/mediabus/mediabus_frame_ring.hpp"

#include "statusbar/test/test.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using namespace statusbar;
using namespace statusbar::mediabus;

namespace {

// Two channels of L24: 6 bytes a frame.
constexpr std::uint32_t FRAME_BYTES = 6;
constexpr std::uint64_t BASE = 4'000'000'000ULL;  // past a 32-bit wrap, like a real index

auto make_config() -> FrameRing::Config
{
    return {.frame_bytes = FRAME_BYTES,
            .block_frames = 6,
            .blocks = 256,  // 1536 frames
            .max_ahead_frames = 960,
            .max_behind_frames = 96};
}

// Frame `index` filled with bytes derived from the index, so a frame read back
// can be checked against where it claims to be from.
auto frames_from(std::uint64_t first, std::size_t count) -> std::vector<std::byte>
{
    std::vector<std::byte> bytes(count * FRAME_BYTES);
    for (std::size_t f = 0; f < count; ++f) {
        for (std::size_t b = 0; b < FRAME_BYTES; ++b) {
            bytes[(f * FRAME_BYTES) + b] = static_cast<std::byte>(((first + f) * 31) + b);
        }
    }
    return bytes;
}

auto frame_is(std::span<std::byte const> bytes, std::size_t f, std::uint64_t index) -> bool
{
    for (std::size_t b = 0; b < FRAME_BYTES; ++b) {
        if (bytes[(f * FRAME_BYTES) + b] != static_cast<std::byte>((index * 31) + b)) {
            return false;
        }
    }
    return true;
}

auto frame_is_silent(std::span<std::byte const> bytes, std::size_t f) -> bool
{
    for (std::size_t b = 0; b < FRAME_BYTES; ++b) {
        if (bytes[(f * FRAME_BYTES) + b] != std::byte{0}) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(mediabus_frame_ring, validate_requires_a_ring_longer_than_the_window)
{
    EXPECT_TRUE(FrameRing::validate(make_config()).has_value());

    auto config = make_config();
    config.blocks = 176;  // 1056 frames: exactly ahead + behind + 2 blocks
    EXPECT_FALSE(FrameRing::validate(config).has_value());
    config.blocks = 177;
    EXPECT_TRUE(FrameRing::validate(config).has_value());

    config = make_config();
    config.block_frames = 33;
    EXPECT_FALSE(FrameRing::validate(config).has_value());
    config = make_config();
    config.frame_bytes = 0;
    EXPECT_FALSE(FrameRing::validate(config).has_value());
}

TEST(mediabus_frame_ring, an_rtp_packet_reads_back_as_aaf_packets)
{
    FrameRing ring{make_config()};
    std::uint64_t const now = BASE;
    // One 1 ms RTP packet (48 frames) at an index that is not block-aligned.
    std::uint64_t const first = now + 100 + 3;
    auto const packet = frames_from(first, 48);
    EXPECT_EQ(ring.write(now, first, packet), std::size_t{48});

    // Eight 6-frame AAF packets cover it.
    for (std::uint64_t p = 0; p < 8; ++p) {
        std::vector<std::byte> out(6 * FRAME_BYTES);
        EXPECT_EQ(ring.read(now, first + (p * 6), out), std::size_t{6});
        for (std::size_t f = 0; f < 6; ++f) {
            EXPECT_TRUE(frame_is(out, f, first + (p * 6) + f));
        }
    }
}

TEST(mediabus_frame_ring, missing_frames_read_as_silence)
{
    FrameRing ring{make_config()};
    std::uint64_t const now = BASE;
    auto const packet = frames_from(now + 10, 4);
    ring.write(now, now + 10, packet);

    std::vector<std::byte> out(12 * FRAME_BYTES, std::byte{0xAA});
    EXPECT_EQ(ring.read(now, now + 6, out), std::size_t{4});
    for (std::size_t f = 0; f < 12; ++f) {
        if (f >= 4 && f < 8) {
            EXPECT_TRUE(frame_is(out, f, now + 6 + f));
        } else {
            EXPECT_TRUE(frame_is_silent(out, f));
        }
    }
}

TEST(mediabus_frame_ring, duplicates_are_not_rewritten)
{
    FrameRing ring{make_config()};
    std::uint64_t const now = BASE;
    auto const packet = frames_from(now, 12);
    EXPECT_EQ(ring.write(now, now, packet), std::size_t{12});

    // A second copy with different bytes (a redundant path, a retransmission) is skipped.
    auto other = frames_from(now + 999, 12);
    EXPECT_EQ(ring.write(now, now, other), std::size_t{0});
    // Overlapping the end: only the new frames are stored.
    auto const tail = frames_from(now + 6, 12);
    EXPECT_EQ(ring.write(now, now + 6, tail), std::size_t{6});

    std::vector<std::byte> out(18 * FRAME_BYTES);
    EXPECT_EQ(ring.read(now, now, out), std::size_t{18});
    for (std::size_t f = 0; f < 18; ++f) {
        EXPECT_TRUE(frame_is(out, f, now + f));
    }
}

TEST(mediabus_frame_ring, the_last_lap_reads_as_silence)
{
    FrameRing ring{make_config()};
    std::uint64_t now = BASE;
    auto const packet = frames_from(now, 6);
    ring.write(now, now, packet);

    // One lap later the same slot is asked for a different block: nothing there.
    now += ring.frames();
    std::vector<std::byte> out(6 * FRAME_BYTES);
    EXPECT_EQ(ring.read(now, now, out), std::size_t{0});
    for (std::size_t f = 0; f < 6; ++f) {
        EXPECT_TRUE(frame_is_silent(out, f));
    }
}

TEST(mediabus_frame_ring, writes_and_reads_outside_the_window_are_refused)
{
    FrameRing ring{make_config()};
    std::uint64_t const now = BASE;
    // Too early (beyond max_ahead) and too late (beyond max_behind).
    auto const early = frames_from(now + 961, 6);
    EXPECT_EQ(ring.write(now, now + 961, early), std::size_t{0});
    auto const late = frames_from(now - 102, 6);
    EXPECT_EQ(ring.write(now, now - 102, late), std::size_t{0});
    // Straddling the far edge: the frames inside are kept.
    auto const edge = frames_from(now + 958, 6);
    EXPECT_EQ(ring.write(now, now + 958, edge), std::size_t{3});

    std::vector<std::byte> out(6 * FRAME_BYTES);
    EXPECT_EQ(ring.read(now, now + 958, out), std::size_t{3});
    EXPECT_TRUE(frame_is(out, 0, now + 958));
    EXPECT_TRUE(frame_is_silent(out, 3));
    // The same frames, read once now has moved on past max_behind: refused.
    EXPECT_EQ(ring.read(now + 958 + 97, now + 958, out), std::size_t{0});
}

TEST(mediabus_frame_ring, view_spans_the_wrap_in_two_parts)
{
    FrameRing ring{make_config()};
    // Put the read range across the end of the storage.
    std::uint64_t const first = (((BASE / ring.frames()) + 1) * ring.frames()) - 3;
    std::uint64_t const now = first;
    auto const packet = frames_from(first, 6);
    ring.write(now, first, packet);

    auto const view = ring.view(now, first, 6);
    EXPECT_TRUE(view.complete);
    EXPECT_EQ(view.parts[0].size(), std::size_t{3 * FRAME_BYTES});
    EXPECT_EQ(view.parts[1].size(), std::size_t{3 * FRAME_BYTES});
    EXPECT_TRUE(frame_is(view.parts[0], 0, first));
    EXPECT_TRUE(frame_is(view.parts[1], 0, first + 3));

    // Incomplete ranges give no view.
    EXPECT_FALSE(ring.view(now, first, 7).complete);
    EXPECT_FALSE(ring.view(now, first + 100, 6).complete);
}

// A writer running ahead of now and a reader trailing it, on separate threads:
// every frame the reader sees must be the frame for its index (no torn or stale
// lap data). Run under TSan this also checks the ring is free of data races.
TEST(mediabus_frame_ring, a_writer_and_a_reader_on_separate_threads)
{
    FrameRing ring{make_config()};
    std::atomic<std::uint64_t> now{BASE};
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> bad{0};
    std::atomic<std::uint64_t> seen{0};

    std::thread writer([&] {
        for (std::uint64_t t = 0; t < 20'000; ++t) {
            std::uint64_t const n = now.load(std::memory_order_relaxed);
            // RTP-sized packets landing a link offset ahead.
            std::uint64_t const first = n + 480 - (n % 48);
            auto const packet = frames_from(first, 48);
            ring.write(n, first, packet);
            now.store(n + 6, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });
    std::thread reader([&] {
        std::vector<std::byte> out(6 * FRAME_BYTES);
        while (!done.load(std::memory_order_acquire)) {
            std::uint64_t const n = now.load(std::memory_order_relaxed);
            std::uint64_t const first = n - (n % 6);
            std::size_t const real = ring.read(n, first, out);
            for (std::size_t f = 0; f < 6; ++f) {
                if (!frame_is_silent(out, f) && !frame_is(out, f, first + f)) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
            seen.fetch_add(real, std::memory_order_relaxed);
        }
    });
    writer.join();
    reader.join();
    EXPECT_EQ(bad.load(), std::uint64_t{0});
    EXPECT_TRUE(seen.load() > 0);
}
