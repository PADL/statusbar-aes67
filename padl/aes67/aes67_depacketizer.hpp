#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// The per-packet rules of an AES67 receiver: which RTP packets are the stream's (payload type, size, source,
/// SSRC learning, RFC 3550 Appendix A.1 sequence check), the reorder window that places them, the interarrival
/// jitter (Appendix A.8) and the counters. Real-time safe; one thread.

#include "padl/aes67/aes67_stream.hpp"
#include "padl/rtp/rtp_header.hpp"
#include "padl/rtp/rtp_sequence.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"

#include <bitset>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

namespace padl::aes67 {

class Depacketizer
{
  public:
    /// Sequence numbers the reorder window remembers: as far back as RtpSequence accepts a late packet.
    static constexpr size_t WINDOW = rtp::RtpSequence::MAX_MISORDER + 1;

    explicit Depacketizer(StreamFormat const& format) noexcept
        : format_{format}
    {}

    /// A sender-clocked stream (true): a new source's packets are refused no_offset, its offset forgotten.
    void set_sender_clock(bool const sender) noexcept { sender_clock_ = sender; }
    [[nodiscard]] auto sender_clock() const noexcept -> bool { return sender_clock_; }
    /// The stream's payload type and the most frames a packet may hold.
    void set_stream(uint8_t const payload_type, uint32_t const max_frames) noexcept
    {
        payload_type_ = payload_type;
        max_frames_ = max_frames;
    }
    /// The one source address admitted (RFC 4570), or any (nullopt).
    void set_source_filter(std::optional<statusbar::ip::IPv4Address> const& source) noexcept { source_filter_ = source; }
    [[nodiscard]] auto source_filter() const noexcept -> std::optional<statusbar::ip::IPv4Address> const& { return source_filter_; }

    /// The packet's frames when it is this stream's, from its source and in sequence; else why not. no_offset: a
    /// sender-clocked stream adopted a new source, whose offset the caller forgets (its sender's reports give it).
    [[nodiscard]] auto admit(rtp::RtpPacket const& packet, statusbar::ip::IPv4Address const& source, uint64_t now_index) noexcept
        -> std::expected<uint32_t, DropReason>;
    /// Notes an admitted packet's arrival: the interarrival jitter (RFC 3550 Appendix A.8) and its timestamp.
    void notify_arrival(uint32_t const timestamp, uint64_t const now_index) noexcept
    {
        // RFC 3550 Appendix A.8 on the first packet of each drain (its index is the arrival time,
        // shared by the batch): transit = arrival - timestamp; J += (|D| - J) / 16, kept x16.
        uint32_t const transit = static_cast<uint32_t>(now_index) - timestamp;
        if (have_transit_ && now_index != last_arrival_index_) {
            auto const d = static_cast<int32_t>(transit - last_transit_);
            uint32_t const magnitude = d < 0 ? 0U - static_cast<uint32_t>(d) : static_cast<uint32_t>(d);
            jitter_x16_ += magnitude - ((jitter_x16_ + 8) >> 4U);
        }
        if (!have_transit_ || now_index != last_arrival_index_) {
            last_transit_ = transit;
            last_arrival_index_ = now_index;
            have_transit_ = true;
        }
        last_rtp_ts_ = timestamp;
    }
    /// Counts a refused packet: dropped, and its reason.
    void drop(DropReason const reason) noexcept
    {
        ++counters_.dropped;
        ++counters_.drops[reason];
    }
    /// Counts a packet of `frames` handed on.
    void delivered(uint32_t const frames) noexcept
    {
        ++counters_.packets;
        counters_.frames += frames;
    }

    /// Forgets the source, its sequence, reorder window and jitter: the next packet starts the stream afresh.
    void reset() noexcept;
    /// Whether admit() restarted the stream (adopted a new source) since the last call.
    [[nodiscard]] auto take_restarted() noexcept -> bool
    {
        if (!restarted_) {
            return false;  // read alone: the drain's common case stores nothing
        }
        restarted_ = false;
        return true;
    }

    [[nodiscard]] auto counters() const noexcept -> Counters const& { return counters_; }
    /// Every counter to zero; the stream, its source and sequence go on.
    void reset_counters() noexcept { counters_ = {}; }
    /// The counters of a stream depacketized elsewhere (a frame port's), for counters() to read.
    void adopt_counters(Counters const& counters) noexcept { counters_ = counters; }
    /// The SSRC learnt from the stream's first packet.
    [[nodiscard]] auto ssrc() const noexcept -> std::optional<uint32_t> const& { return ssrc_; }
    /// The report block inputs (RFC 3550 Section 6.4.1) once a source is learnt, as of `now_index`.
    [[nodiscard]] auto receive_state(uint64_t const now_index) const noexcept -> ReceiveState
    {
        return {
            .ssrc = ssrc_.value_or(0),
            .valid = ssrc_.has_value(),
            .source = source_,
            .extended_max = sequence_.extended_max(),
            .expected = sequence_.expected(),
            .received = sequence_.received(),
            .jitter_x16 = jitter_x16_,
            .last_rtp_ts = last_rtp_ts_,
            .last_now_index = now_index,
        };
    }

  private:
    /// Why the packet's payload type, size or source is not this stream's; nullopt when it is.
    [[nodiscard]] auto check_payload(rtp::RtpPacket const& packet, statusbar::ip::IPv4Address const& source) const noexcept
        -> std::optional<DropReason>;
    /// Learns the SSRC and its address, or adopts a new pair after the learnt source fell silent: the frames since the
    /// source's last packet (0 for a new one, or after a clock step back); else why the packet is dropped.
    [[nodiscard]] auto admit_source(uint32_t ssrc, statusbar::ip::IPv4Address const& source, uint64_t now_index) noexcept
        -> std::expected<uint64_t, DropReason>;
    /// Validates the sequence number (RFC 3550 Appendix A.1) and places it, the sequence restarted first after a silence
    /// of `silent` frames past RtpSequence::MAX_DROPOUT packets; false to drop the packet.
    [[nodiscard]] auto admit_sequence(uint16_t sequence, uint32_t frames, uint64_t silent) noexcept -> bool;
    /// Accounts a validated packet's place in the sequence; false to drop it.
    [[nodiscard]] auto place(uint16_t sequence, uint32_t frames) noexcept -> bool;
    /// Forgets the sequence and its reorder window: the next packet starts them afresh, counting nothing lost.
    void restart_sequence() noexcept;
    /// Restarts the reorder window at `sequence`.
    void start_window(uint16_t sequence) noexcept;
    /// Moves the window `ahead` to `sequence`, counting the packets skipped as lost.
    void advance_window(uint16_t sequence, uint16_t ahead, uint32_t frames) noexcept;
    /// Marks a late packet `behind` the newest; false for a duplicate.
    [[nodiscard]] auto mark_late(uint16_t behind, uint32_t frames) noexcept -> bool;

    StreamFormat format_;
    uint8_t payload_type_{0};
    uint32_t max_frames_{0};
    bool sender_clock_{false};
    bool restarted_{false};
    std::optional<statusbar::ip::IPv4Address> source_filter_;

    std::optional<uint32_t> ssrc_;
    statusbar::ip::IPv4Address source_;  ///< the address ssrc_ was learnt from
    uint64_t last_index_{0};             ///< now_index when the learnt source last sent a packet accepted
    rtp::RtpSequence sequence_;
    bool have_max_{false};
    uint16_t max_seq_{0};
    /// Bit i: max_seq_ - i received, for i up to RtpSequence::MAX_MISORDER.
    std::bitset<128> window_;
    Counters counters_;

    // RFC 3550 Appendix A.8 interarrival jitter.
    uint32_t jitter_x16_{0};
    uint32_t last_transit_{0};
    uint64_t last_arrival_index_{0};
    bool have_transit_{false};
    uint32_t last_rtp_ts_{0};
};

}  // namespace padl::aes67
