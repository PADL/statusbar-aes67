// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/aes67/aes67_packet_schedule.hpp"

#include <random>

namespace padl::aes67 {

void PacketSchedule::seed()
{
    std::random_device random;
    rng_.seed((uint64_t{random()} << 32U) | random());
    ssrc_.store(fresh_ssrc(), std::memory_order_relaxed);  // never 0, nor the last run's
    sequence_ = static_cast<uint16_t>(random());
}

auto PacketSchedule::held_first(uint64_t const newest) const noexcept -> uint64_t
{
    uint64_t const fpp = format_.frames_per_packet;
    uint64_t const lead = ((uint64_t{format_.sample_rate} * FIRST_REPORT_LEAD_US) / 1'000'000) + fpp - 1;
    return newest + ((lead / fpp) * fpp);
}

auto PacketSchedule::start_held(uint64_t const newest) noexcept -> bool
{
    uint64_t const burst = uint64_t{MAX_BURST - 1} * format_.frames_per_packet;
    if (!held_first_ || newest > *held_first_ + burst || *held_first_ > held_first(newest)) {
        held_first_ = held_first(newest);
        placed_ = true;
        return false;
    }
    if (newest < *held_first_) {
        return false;  // its SR has until then; the run starts then whether or not it went
    }
    next_index_ = run_first_ = *held_first_;
    started_ = true;
    return true;
}

auto PacketSchedule::newest_due(uint64_t const now_index, uint64_t const send_offset, bool const hold, uint64_t& lost) noexcept
    -> std::optional<uint64_t>
{
    uint64_t const fpp = format_.frames_per_packet;
    uint64_t const phase = phase_;
    if (now_index + 1 < send_offset + fpp + phase) {
        return std::nullopt;  // no packet due yet (only near the epoch)
    }
    // The newest packet (on the grid shifted by the phase) whose last frame is send_offset or more behind now.
    uint64_t const newest = ((((now_index + 1 - send_offset - phase) / fpp) - 1) * fpp) + phase;
    return resync(newest, hold, lost) ? std::optional{newest} : std::nullopt;
}

auto PacketSchedule::resync(uint64_t const newest, bool const hold, uint64_t& lost) noexcept -> bool
{
    uint64_t const fpp = format_.frames_per_packet;
    if (!started_ && hold && !start_held(newest)) {
        return false;  // the run's first packet waits for its SR
    }
    if (!started_) {
        next_index_ = run_first_ = newest;
        started_ = true;
    } else if (next_index_ > newest + fpp) {
        // The clock went back. A small step is waited out (nothing is due
        // until now catches up, so no index is sent twice); a large one,
        // more than MAX_BURST packets, resynchronises.
        if (next_index_ - (newest + fpp) <= uint64_t{MAX_BURST} * fpp) {
            return false;
        }
        next_index_ = newest;
    } else if (next_index_ + (uint64_t{MAX_BURST} * fpp) <= newest) {
        lost += newest - next_index_;  // too far behind: skip what cannot be sent in time
        next_index_ = newest;
    }
    return true;
}

auto PacketSchedule::next_random() noexcept -> uint32_t
{
    return static_cast<uint32_t>(rng_() >> 32U);
}

auto PacketSchedule::fresh_ssrc() noexcept -> uint32_t
{
    uint32_t const old = ssrc();
    uint32_t next = next_random();
    while (next == 0 || next == old) {
        next = next_random();
    }
    return next;
}

}  // namespace padl::aes67
