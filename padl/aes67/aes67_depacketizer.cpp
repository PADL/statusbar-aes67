// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/aes67/aes67_depacketizer.hpp"

#include <algorithm>

namespace padl::aes67 {

static_assert(Depacketizer::WINDOW <= 128);

void Depacketizer::reset() noexcept
{
    ssrc_.reset();
    source_ = {};
    restart_sequence();
    jitter_x16_ = 0;
    have_transit_ = false;
}

void Depacketizer::restart_sequence() noexcept
{
    sequence_ = rtp::RtpSequence{};
    have_max_ = false;
    max_seq_ = 0;
    window_.reset();
    jump_lost_ = 0;
}

auto Depacketizer::admit(rtp::RtpPacket const& packet, statusbar::ip::IPv4Address const& source, uint64_t const now_index) noexcept
    -> std::expected<uint32_t, DropReason>
{
    if (auto const refused = check_payload(packet, source)) {
        return std::unexpected{*refused};
    }
    auto const silent = admit_source(packet.header.ssrc.get(), source, now_index);
    if (!silent) {
        return std::unexpected{silent.error()};
    }
    auto const frames = static_cast<uint32_t>(packet.payload.size() / format_.bytes_per_frame());
    if (!admit_sequence(packet.header.sequence.get(), frames, *silent)) {
        return std::unexpected{DropReason::sequence};
    }
    return frames;
}

auto Depacketizer::check_payload(rtp::RtpPacket const& packet, statusbar::ip::IPv4Address const& source) const noexcept
    -> std::optional<DropReason>
{
    if (packet.header.payload_type() != payload_type_) {
        return DropReason::payload_type;
    }
    size_t const bytes = packet.payload.size();
    if (bytes == 0 || bytes % format_.bytes_per_frame() != 0 || bytes / format_.bytes_per_frame() > max_frames_) {
        return DropReason::size;
    }
    if (source_filter_ && source != *source_filter_) {
        return DropReason::source;
    }
    return std::nullopt;
}

auto Depacketizer::admit_source(uint32_t const ssrc, statusbar::ip::IPv4Address const& source, uint64_t const now_index) noexcept
    -> std::expected<uint64_t, DropReason>
{
    if (ssrc_ && (*ssrc_ != ssrc || source_ != source)) {
        // Another SSRC or address: refused while the learnt source is live, adopted once it has been silent for a
        // second (a restarted sender). Signed: after a PTP step backwards the learnt source still counts as live.
        if (static_cast<int64_t>(now_index - last_index_) < static_cast<int64_t>(format_.sample_rate)) {
            return std::unexpected{*ssrc_ != ssrc ? DropReason::ssrc : DropReason::source};
        }
        reset();
        restarted_ = true;
        if (sender_clock_) {
            return std::unexpected{DropReason::no_offset};  // the new source's reports give its own offset
        }
    }
    auto const silent = ssrc_ ? std::max<int64_t>(static_cast<int64_t>(now_index - last_index_), 0) : 0;
    if (!ssrc_) {
        ssrc_ = ssrc;
        source_ = source;
    }
    last_index_ = now_index;
    return static_cast<uint64_t>(silent);
}

auto Depacketizer::admit_sequence(uint16_t const seq, uint32_t const frames, uint64_t const silent) noexcept -> bool
{
    // Silent for more than MAX_DROPOUT packets, the source restarted or wrapped: no step tells what was lost.
    if (silent > uint64_t{rtp::RtpSequence::MAX_DROPOUT} * frames) {
        restart_sequence();
    }
    bool const was_validated = sequence_.validated();
    auto const step = static_cast<uint16_t>(seq - static_cast<uint16_t>(sequence_.extended_max()));
    if (!sequence_.update(seq) && was_validated) {
        return false;  // an unconfirmed jump
    }
    // A packet on probation (a new source's first) is handed on all the same, nothing counted lost:
    // its timestamp, not its sequence number, places it, and index_of_rtp() puts it about now.
    return place(seq, frames, notify_step(was_validated, seq, step));
}

auto Depacketizer::notify_step(bool const was_validated, uint16_t const seq, uint16_t const step) noexcept -> bool
{
    constexpr uint32_t SEQ_MOD = 1U << 16U;
    if (was_validated && step >= rtp::RtpSequence::MAX_DROPOUT && step <= SEQ_MOD - rtp::RtpSequence::MAX_MISORDER) {
        // RFC 3550 A.1 confirmed a jump back to the sequence the newest step left: its gap was another's, not loss.
        auto const resumed = static_cast<uint16_t>(seq - jump_from_) < rtp::RtpSequence::MAX_DROPOUT;
        counters_.lost -= resumed ? std::min(counters_.lost, jump_lost_) : 0;
        jump_lost_ = 0;
    }
    return was_validated;
}

auto Depacketizer::place(uint16_t const seq, uint32_t const frames, bool const counted) noexcept -> bool
{
    auto const ahead = static_cast<uint16_t>(seq - max_seq_);
    auto const behind = static_cast<uint16_t>(max_seq_ - seq);
    if (have_max_ && ahead != 0 && ahead < rtp::RtpSequence::MAX_DROPOUT) {
        advance_window(seq, ahead, frames, counted);
        return true;
    }
    if (have_max_ && behind < WINDOW) {  // up to RtpSequence::MAX_MISORDER behind, as it accepts
        return mark_late(behind, frames);
    }
    start_window(seq);  // the first packet, or a jump RtpSequence confirmed: the sender restarted its sequence
    return true;
}

void Depacketizer::start_window(uint16_t const seq) noexcept
{
    have_max_ = true;
    max_seq_ = seq;
    window_.reset();
    window_.set(0);
}

void Depacketizer::advance_window(uint16_t const seq, uint16_t const ahead, uint32_t const frames, bool const counted) noexcept
{
    uint64_t const gap = counted ? uint64_t{ahead - 1U} * frames : 0;  // assumes the stream's packets are all this size
    counters_.lost += gap;
    notify_jump(ahead, gap);
    if (ahead >= WINDOW) {
        window_.reset();
    } else {
        window_ <<= ahead;
    }
    window_.set(0);
    max_seq_ = seq;
}

void Depacketizer::notify_jump(uint16_t const ahead, uint64_t const gap) noexcept
{
    if (ahead >= WINDOW) {
        jump_from_ = max_seq_;
        jump_lost_ = gap;  // an earlier step's loss stays lost
        since_jump_ = 0;
        return;
    }
    if (since_jump_ < rtp::RtpSequence::MAX_MISORDER) {
        ++since_jump_;
        jump_lost_ = since_jump_ == rtp::RtpSequence::MAX_MISORDER ? 0 : jump_lost_;  // went on: its gap was loss
    }
}

auto Depacketizer::mark_late(uint16_t const behind, uint32_t const frames) noexcept -> bool
{
    if (window_.test(behind)) {
        return false;  // a duplicate
    }
    window_.set(behind);
    counters_.late += frames;
    counters_.lost -= std::min<uint64_t>(counters_.lost, frames);
    jump_lost_ -= std::min<uint64_t>(jump_lost_, frames);  // recovered: not withdrawn again
    return true;
}

}  // namespace padl::aes67
