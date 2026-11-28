#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// Test support: an in-memory Transport (with frame ports that run a Depacketizer and a PacketSchedule, as an engine
/// beside the network would), a pattern source and a recording sink, and crafted sessions and RTP packets, for the
/// tests of AES67 senders and receivers.

#include "padl/aes67/aes67_depacketizer.hpp"
#include "padl/aes67/aes67_packet_schedule.hpp"
#include "padl/aes67/aes67_plan.hpp"
#include "padl/aes67/aes67_receiver.hpp"
#include "padl/aes67/aes67_sender.hpp"
#include "padl/aes67/aes67_transport.hpp"
#include "padl/mediaclock/mediaclock_ptp_clock.hpp"
#include "padl/rtp/rtp_header.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace padl::aes67 {

/// Frames per packet: a 1 ms ptime at 48 kHz.
inline constexpr uint32_t FPP = 48;

/// A loopback address of this process's own, 127.(1-64).x.y from the pid: the suite runs in
/// several processes at once (ctest -j, one per file and one for all), and receivers bind their
/// destination address, so no other process's datagrams reach these tests' ports.
inline auto own_loopback() -> statusbar::ip::IPv4Address
{
    auto const pid = static_cast<uint32_t>(::getpid());
    return statusbar::ip::IPv4Address{
        127, static_cast<uint8_t>(1 + ((pid >> 16U) % 64)), static_cast<uint8_t>(pid >> 8U), static_cast<uint8_t>(pid)};
}

/// A multicast group of this process's own (239.(64-127).x.y from the pid), for the same reason.
inline auto own_group() -> statusbar::ip::IPv4Address
{
    auto const pid = static_cast<uint32_t>(::getpid());
    return statusbar::ip::IPv4Address{
        239, static_cast<uint8_t>(64 + ((pid >> 16U) % 64)), static_cast<uint8_t>(pid >> 8U), static_cast<uint8_t>(pid)};
}

/// A PTP grandmaster in domain 0, for senders to name.
inline Grandmaster const GRANDMASTER{.identity = *statusbar::ieee::eui64_from_string("00-1D-C1-FF-FE-12-34-56"), .domain = 0};

/// Two channels of L24 at 48 kHz, FPP frames per packet.
inline constexpr StreamFormat STEREO_L24{
    .encoding = sdp::Encoding::l24, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP};
/// Two channels of L32 at 48 kHz, FPP frames per packet.
inline constexpr StreamFormat STEREO_L32{
    .encoding = sdp::Encoding::l32, .channels = 2, .sample_rate = 48000, .frames_per_packet = FPP};

/// Byte `b` of frame `index`: a pattern a receiver can check by index.
inline auto pattern(uint64_t const index, size_t const b) -> uint8_t
{
    return static_cast<uint8_t>((index * 7) + b);
}

/// Generates the pattern for any index, or refuses indices in `missing`.
class PatternSource
{
  public:
    /// A source of `frame_bytes` octets a frame.
    explicit PatternSource(uint32_t const frame_bytes)
        : frame_bytes_{frame_bytes}
    {}

    /// A FrameProvider that pull()s from this source, which must outlive it.
    auto provider() -> FrameProvider
    {
        return [this](uint64_t const first_index, uint32_t const frames, std::span<uint8_t> const out) {
            return pull(first_index, frames, out) ? Supply::frames : Supply::missing;
        };
    }

    /// `frames` frames of the pattern from `first_index` into `out`; false for refuse_index.
    auto pull(uint64_t const first_index, uint32_t const frames, std::span<uint8_t> const out) noexcept -> bool
    {
        ++pulls;
        if (first_index == refuse_index) {
            return false;
        }
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t b = 0; b < frame_bytes_; ++b) {
                out[(size_t{f} * frame_bytes_) + b] = pattern(first_index + f, b);
            }
        }
        return true;
    }

    uint64_t refuse_index{UINT64_MAX};  ///< the first index pull() refuses
    size_t pulls{0};                    ///< pull() calls

  private:
    uint32_t frame_bytes_;
};

/// A block a receiver handed on: its first index, its frame count and a copy of its bytes.
struct Received
{
    uint64_t first_index;        ///< the index of its first frame
    uint32_t frames;             ///< its frame count
    std::vector<uint8_t> bytes;  ///< its payload
};

/// Records every block a receiver hands on.
class RecordingSink
{
  public:
    /// A FrameHandler appending to blocks; this sink must outlive it.
    auto handler() -> FrameHandler
    {
        return [this](uint64_t const first_index, uint32_t const frames, std::span<uint8_t const> const bytes) {
            blocks.push_back({first_index, frames, {bytes.begin(), bytes.end()}});
        };
    }
    std::vector<Received> blocks;  ///< every block, in order
};

/// A transport in memory: what senders send, receivers receive.
class LoopTransport final : public Transport
{
  public:
    /// A packet a send port sent, for the receive ports of its destination.
    struct Packet
    {
        Endpoint destination;                            ///< where it was sent
        std::vector<uint8_t> bytes;                      ///< the whole RTP packet
        std::optional<statusbar::ieee::VlanTag> vlan{};  ///< the port's tag when it was sent
    };

    /// A receive port: the queued packets for its destination, from `source`.
    class Rx final : public ReceivePort
    {
      public:
        /// A port on `t` for `d`; `t` must outlive it.
        Rx(LoopTransport& t, Endpoint const& d)
            : transport{t}
            , destination{d}
        {}
        /// The queued packets for its destination, others dropped; each held until the next call.
        auto receive(std::span<Datagram> const out) noexcept -> size_t override
        {
            ++transport.receives;
            held.clear();
            size_t n = 0;
            while (n < out.size() && !transport.queue.empty()) {
                auto packet = std::move(transport.queue.front());
                transport.queue.pop_front();
                if (packet.destination != destination) {
                    continue;
                }
                held.push_back(std::move(packet.bytes));
                out[n++] = {.payload = held.back(), .source = transport.source};
            }
            return n;
        }
        /// The transport's port_error.
        [[nodiscard]] auto error() const noexcept -> std::error_code override { return transport.port_error; }
        LoopTransport& transport;               ///< where its packets come from
        Endpoint destination;                   ///< what it receives
        std::deque<std::vector<uint8_t>> held;  ///< the datagrams the last receive() returned
    };

    /// A send port: every packet queued for its destination.
    class Tx final : public SendPort
    {
      public:
        /// A port on `t` to `d`; `t` must outlive it.
        Tx(LoopTransport& t, Endpoint const& d)
            : transport{t}
            , destination{d}
        {}
        /// The packet queued, or false (render not called) when the transport refuses.
        auto send_rendered(rtp::RtpHeader const& header, size_t const payload_length, PayloadRenderer const& render) noexcept
            -> bool override
        {
            if (transport.refuse) {
                return false;  // no TX buffer: render is not called
            }
            std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + payload_length);
            auto const offset = rtp::serialize_rtp_header(header, payload_length, bytes);
            if (!offset || !render(std::span{bytes}.subspan(*offset, payload_length))) {
                return false;
            }
            transport.queue.push_back({destination, std::move(bytes), transport.vlan});
            return true;
        }
        /// send_rendered(), its departure recorded in departures.
        auto send_rendered_at(
            rtp::RtpHeader const& header,
            size_t const payload_length,
            PayloadRenderer const& render,
            uint64_t const departure_ns) noexcept -> bool override
        {
            transport.departures.push_back(departure_ns);
            return send_rendered(header, payload_length, render);
        }
        /// Nothing batched: 0.
        auto flush() noexcept -> size_t override { return 0; }
        /// Under packet_gather, `routes` accepted and recorded in gathered; else the default.
        auto gather_from(std::span<FrameRoute const> const routes) -> statusbar::Status override
        {
            if (!transport.packet_gather) {
                return SendPort::gather_from(routes);
            }
            transport.gathered.assign(routes.begin(), routes.end());
            return statusbar::success();
        }
        /// While gathering, run 1 with frame_lost frames found missing; else none.
        auto progress() const noexcept -> SendProgress override
        {
            return transport.gathered.empty() ? SendProgress{} : SendProgress{.ssrc = 1, .lost = transport.frame_lost};
        }
        /// The tag recorded in vlan; refused, as a socket refuses it, under refuse_tag.
        auto set_vlan(std::optional<statusbar::ieee::VlanTag> const& vlan) noexcept -> statusbar::Status override
        {
            if (transport.refuse_tag && vlan) {
                return SendPort::set_vlan(vlan);
            }
            transport.vlan = vlan;
            return statusbar::success();
        }
        LoopTransport& transport;  ///< where its packets go
        Endpoint destination;      ///< where it sends
    };

    /// Frames a frame port holds, at index modulo this: not a multiple of FPP, so packets wrap.
    static constexpr uint32_t FRAME_RING = 1000;

    /// A frame receive port (frame_ports): the queued packets for its plan's destination admitted by a Depacketizer
    /// and placed by its offset into a ring of FRAME_RING frames, as an engine would; each drain hands the frames it
    /// placed on from the ring.
    class FrameRx final
        : public ReceivePort
        , public FrameReceive
    {
      public:
        /// A port on `t` for `p`; `t` must outlive it.
        FrameRx(LoopTransport& t, ReceivePlan const& p)
            : transport{t}
            , plan{p}
            , depacketizer{p.format}
            , offset{p.offset}
            , ring(size_t{FRAME_RING} * p.format.bytes_per_frame())
        {
            depacketizer.set_sender_clock(p.sender_clock);
            depacketizer.set_stream(p.payload_type, p.max_frames);
            depacketizer.set_source_filter(p.source_filter);
        }
        ~FrameRx() override
        {
            if (transport.frame_rx == this) {
                transport.frame_rx = nullptr;
            }
        }
        FrameRx(FrameRx const&) = delete;
        auto operator=(FrameRx const&) -> FrameRx& = delete;
        FrameRx(FrameRx&&) = delete;
        auto operator=(FrameRx&&) -> FrameRx& = delete;
        /// Not a packet port: nothing.
        auto receive(std::span<Datagram> const /*out*/) noexcept -> size_t override { return 0; }
        /// The transport's port_error.
        [[nodiscard]] auto error() const noexcept -> std::error_code override { return transport.port_error; }
        auto frames() noexcept -> FrameReceive* override { return this; }

        /// Every queued packet for the destination placed, then the frames placed handed on.
        auto drain(uint64_t const now_index, FrameHandler const& handler) noexcept -> FrameDrain override
        {
            FrameDrain drained{};
            placed.clear();
            for (auto bytes = next_packet(); bytes; bytes = next_packet()) {
                if (!is_multiplexed_rtcp(*bytes)) {
                    accept(*bytes, now_index, drained);  // the stream's own RTCP is not a refused packet
                }
            }
            last_now = now_index;
            for (auto const& [first, frames] : placed) {
                hand_on(first, frames, handler);
            }
            return drained;
        }
        [[nodiscard]] auto counters() const noexcept -> Counters const& override { return depacketizer.counters(); }
        void reset_counters() noexcept override { depacketizer.reset_counters(); }
        /// The Depacketizer's, as of the last drain.
        [[nodiscard]] auto receive_state() const noexcept -> ReceiveState override { return depacketizer.receive_state(last_now); }
        void set_offset(std::optional<uint32_t> const o) noexcept override
        {
            offset = o;
            offsets.push_back(o);
        }

        LoopTransport& transport;                           ///< where its packets come from
        ReceivePlan plan;                                   ///< what it was opened for
        Depacketizer depacketizer;                          ///< its admission and placement
        std::optional<uint32_t> offset;                     ///< the offset it places by
        std::vector<uint8_t> ring;                          ///< FRAME_RING frames
        std::vector<std::pair<uint64_t, uint32_t>> placed;  ///< this drain's placed frames: first index, count
        uint64_t last_now{0};                               ///< the last drain's index
        std::vector<std::optional<uint32_t>> offsets;       ///< every set_offset()

      private:
        /// The next queued packet for the destination, others dropped.
        auto next_packet() -> std::optional<std::vector<uint8_t>>
        {
            while (!transport.queue.empty()) {
                auto packet = std::move(transport.queue.front());
                transport.queue.pop_front();
                if (packet.destination == plan.destination) {
                    return std::move(packet.bytes);
                }
            }
            return std::nullopt;
        }
        /// Receiver::accept()'s steps, with the Depacketizer, placing into the ring.
        void accept(std::vector<uint8_t> const& bytes, uint64_t const now_index, FrameDrain& drained)
        {
            auto const packet = rtp::parse_rtp(bytes);
            auto const frames =
                packet ? depacketizer.admit(*packet, transport.source.address, now_index) : std::unexpected{DropReason::not_rtp};
            if (!packet || !frames) {
                drained.offset_forgotten = drained.offset_forgotten || frames.error() == DropReason::no_offset;
                offset = frames.error() == DropReason::no_offset ? std::nullopt : offset;
                depacketizer.drop(frames.error());
                return;
            }
            depacketizer.notify_arrival(packet->header.timestamp.get(), now_index);
            drained.accepted = true;
            place(*packet, *frames, now_index, drained);
        }
        /// The packet's frames into the ring at their index, or dropped no_offset without an offset.
        void place(rtp::RtpPacket const& packet, uint32_t const frames, uint64_t const now_index, FrameDrain& drained)
        {
            if (!offset) {
                depacketizer.drop(DropReason::no_offset);
                return;
            }
            uint64_t const first = mediaclock::PtpMediaClock::index_of_rtp(packet.header.timestamp.get(), *offset, now_index);
            write_ring(first, frames, packet.payload);
            placed.emplace_back(first, frames);
            depacketizer.delivered(frames);
            ++drained.packets;
        }
        /// `frames` frames of `payload` into the ring from index `first`.
        void write_ring(uint64_t const first, uint32_t const frames, std::span<uint8_t const> const payload)
        {
            size_t const bytes = plan.format.bytes_per_frame();
            for (uint32_t f = 0; f < frames; ++f) {
                statusbar::span_copy(
                    std::span{ring}.subspan(((first + f) % FRAME_RING) * bytes, bytes), payload.subspan(f * bytes, bytes));
            }
        }
        /// Frames [first, first + frames) from the ring to `handler`, in two pieces where the ring wraps.
        void hand_on(uint64_t const first, uint32_t const frames, FrameHandler const& handler) const
        {
            size_t const bytes = plan.format.bytes_per_frame();
            auto const at = static_cast<uint32_t>(first % FRAME_RING);
            uint32_t const n = std::min(frames, FRAME_RING - at);
            handler(first, n, std::span<uint8_t const>{ring}.subspan(size_t{at} * bytes, size_t{n} * bytes));
            if (n < frames) {
                handler(first + n, frames - n, std::span<uint8_t const>{ring}.first(size_t{frames - n} * bytes));
            }
        }
    };

    /// A frame send port (frame_ports): frames written to a ring of FRAME_RING frames; send_due() sends the run by a
    /// PacketSchedule of its plan, each packet its committed frames or silence (none committed: left out). As a device
    /// would, it accepts a run start() gave at its next send_due(), so progress() describes the run before until then.
    class FrameTx final
        : public SendPort
        , public FrameSend
    {
      public:
        static constexpr uint64_t NONE = UINT64_MAX;  ///< a ring slot with no frame committed

        /// A port on `t` for `p`; `t` must outlive it.
        FrameTx(LoopTransport& t, SendPlan const& p)
            : transport{t}
            , plan{p}
            , schedule{p.format}
            , ring(size_t{FRAME_RING} * p.format.bytes_per_frame())
            , slots(FRAME_RING, NONE)
        {
            schedule.set_phase(p.phase);
        }
        ~FrameTx() override
        {
            if (transport.frame_tx == this) {
                transport.frame_tx = nullptr;
            }
        }
        FrameTx(FrameTx const&) = delete;
        auto operator=(FrameTx const&) -> FrameTx& = delete;
        FrameTx(FrameTx&&) = delete;
        auto operator=(FrameTx&&) -> FrameTx& = delete;

        /// Not a packet port: refused.
        auto send_rendered(rtp::RtpHeader const& /*header*/, size_t const /*length*/, PayloadRenderer const& /*render*/) noexcept
            -> bool override
        {
            return false;
        }
        auto flush() noexcept -> size_t override { return 0; }
        auto frames() noexcept -> FrameSend* override { return this; }

        [[nodiscard]] auto start_lead_frames() const noexcept -> uint32_t override { return transport.start_lead; }
        void start(SendRun const& run) noexcept override
        {
            runs.push_back(run);
            pending = run;
            send_offset = run.send_offset;
        }
        void stop() noexcept override
        {
            ++stops;
            pending.reset();
            schedule.stop();
        }
        void set_send_offset(uint32_t const frames) noexcept override { send_offset = frames; }
        /// The ring from `first_index` to its end at most; nothing under the transport's refuse.
        auto writable(uint64_t const first_index, uint32_t const frames) noexcept -> std::span<uint8_t> override
        {
            size_t const bytes = plan.format.bytes_per_frame();
            auto const at = static_cast<uint32_t>(first_index % FRAME_RING);
            uint32_t const n = transport.refuse ? 0 : std::min(frames, FRAME_RING - at);
            return std::span{ring}.subspan(size_t{at} * bytes, size_t{n} * bytes);
        }
        auto commit(uint64_t const first_index, uint32_t const frames) noexcept -> bool override
        {
            for (uint64_t i = first_index; i < first_index + frames; ++i) {
                slots[i % FRAME_RING] = i;
            }
            return true;
        }
        /// The run accepted: what it has sent, and its next packet (its first until that is due).
        [[nodiscard]] auto progress() const noexcept -> SendProgress override
        {
            if (!accepted) {
                return {};
            }
            uint64_t const next = schedule.started() ? schedule.next_index() : accepted->first_index;
            return {
                .ssrc = accepted->ssrc,
                .first_index = accepted->first_index,
                .next_index = next,
                .packets = sent,
                .lost = transport.frame_lost};
        }
        /// The transport's frame_error.
        [[nodiscard]] auto error() const noexcept -> std::error_code override { return transport.frame_error; }

        /// The engine at `now_index`: a run given is accepted, started once its first packet is due, then every packet due
        /// sent.
        void send_due(uint64_t const now_index)
        {
            if (pending && accepted != pending) {
                accepted = pending;
                sent = 0;
            }
            if (pending && schedule.is_due(pending->first_index, now_index, send_offset)) {
                schedule.start(*pending);
                pending.reset();
            }
            if (!schedule.started()) {
                return;
            }
            auto const newest = schedule.newest_due(now_index, send_offset, false, lost);
            for (; newest && schedule.next_index() <= *newest; schedule.advance()) {
                if (any_committed(schedule.next_index())) {
                    send_packet();  // else the Sender dropped it: left out, its sequence number spent
                }
            }
        }

        LoopTransport& transport;         ///< where its packets go
        SendPlan plan;                    ///< what it was opened for
        PacketSchedule schedule;          ///< the run's grid, sequence and SSRC
        std::vector<uint8_t> ring;        ///< FRAME_RING frames
        std::vector<uint64_t> slots;      ///< the index committed in each ring slot, or NONE
        std::optional<SendRun> pending;   ///< a run given, its first packet not yet due
        std::optional<SendRun> accepted;  ///< the run the engine accepted last
        std::vector<SendRun> runs;        ///< every start()
        size_t stops{0};                  ///< stop() calls
        uint32_t send_offset{0};          ///< the run's
        uint32_t sent{0};                 ///< packets sent in the run accepted
        uint64_t lost{0};                 ///< frames sent as silence or skipped

      private:
        /// The packet at the schedule's next index, to the queue.
        void send_packet()
        {
            size_t const payload = plan.format.bytes_per_packet();
            std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + payload);
            (void)rtp::serialize_rtp_header(schedule.header(plan.payload_type, schedule.ssrc()), payload, bytes);
            gather(std::span{bytes}.subspan(rtp::RtpHeader::LENGTH));
            transport.queue.push_back({plan.destination, std::move(bytes), plan.vlan});
            ++sent;
        }
        /// Whether any frame of the packet at `first` was committed.
        [[nodiscard]] auto any_committed(uint64_t const first) const -> bool
        {
            for (uint64_t i = first; i < first + plan.format.frames_per_packet; ++i) {
                if (slots[i % FRAME_RING] == i) {
                    return true;
                }
            }
            return false;
        }
        /// The packet's frames from the ring, or silence (counted lost) when any is not committed.
        void gather(std::span<uint8_t> const payload)
        {
            size_t const bytes = plan.format.bytes_per_frame();
            uint64_t const first = schedule.next_index();
            for (uint32_t f = 0; f < plan.format.frames_per_packet; ++f) {
                if (slots[(first + f) % FRAME_RING] != first + f) {
                    fill_silence(plan.format.encoding, payload);
                    lost += plan.format.frames_per_packet;
                    return;
                }
                statusbar::span_copy(
                    payload.subspan(f * bytes, bytes),
                    std::span<uint8_t const>{ring}.subspan(((first + f) % FRAME_RING) * bytes, bytes));
            }
        }
    };

    /// The source address its packets arrive from.
    [[nodiscard]] auto local_address() const noexcept -> statusbar::ip::IPv4Address override { return source.address; }
    /// An Rx for `destination`, joined for any source.
    auto open_receive(Endpoint const& destination) -> statusbar::StatusValue<std::unique_ptr<ReceivePort>> override
    {
        joined_source.reset();
        return std::make_unique<Rx>(*this, destination);
    }
    /// An Rx for `destination`, joined for `from` alone (joined_source).
    auto open_receive_from(Endpoint const& destination, statusbar::ip::IPv4Address const& from)
        -> statusbar::StatusValue<std::unique_ptr<ReceivePort>> override
    {
        joined_source = from;
        return std::make_unique<Rx>(*this, destination);
    }
    /// A Tx to the options' destination, with their tag.
    auto open_send(SendOptions const& options) -> statusbar::StatusValue<std::unique_ptr<SendPort>> override
    {
        vlan = options.vlan;
        return std::make_unique<Tx>(*this, options.destination);
    }
    /// The plan recorded; a FrameRx (frame_rx) under frame_ports, else the default (an Rx).
    auto open_receive_stream(ReceivePlan const& plan) -> statusbar::StatusValue<std::unique_ptr<ReceivePort>> override
    {
        receive_plans.push_back(plan);
        if (!frame_ports) {
            return Transport::open_receive_stream(plan);
        }
        auto port = std::make_unique<FrameRx>(*this, plan);
        frame_rx = port.get();
        return port;
    }
    /// The plan recorded; a FrameTx (frame_tx) under frame_ports, else the default (a Tx).
    auto open_send_stream(SendPlan const& plan) -> statusbar::StatusValue<std::unique_ptr<SendPort>> override
    {
        send_plans.push_back(plan);
        if (!frame_ports) {
            return Transport::open_send_stream(plan);
        }
        vlan = plan.vlan;
        auto port = std::make_unique<FrameTx>(*this, plan);
        frame_tx = port.get();
        return port;
    }

    /// failing.
    [[nodiscard]] auto send_error() const noexcept -> std::error_code override { return failing; }
    /// every_tick.
    [[nodiscard]] auto drain_every_tick() const noexcept -> bool override { return every_tick; }

    std::deque<Packet> queue;          ///< sent and not yet received
    std::vector<uint64_t> departures;  ///< each packet's departure as the sender gave it
    /// The address and port every packet arrives from.
    Endpoint source{.address = statusbar::ip::IPv4Address{192, 168, 1, 10}, .port = 5004};
    bool refuse{false};                                       ///< every send_rendered() finds no TX buffer
    std::error_code failing;                                  ///< send_error()
    std::error_code port_error;                               ///< every receive port's error()
    std::error_code frame_error;                              ///< every FrameTx's error()
    uint32_t frame_lost{0};                                   ///< every FrameTx's progress(): frames it found missing
    bool packet_gather{false};                                ///< a Tx's gather_from() accepts routes
    std::vector<FrameRoute> gathered;                         ///< the routes a Tx gathers from
    bool every_tick{false};                                   ///< drain_every_tick()
    bool refuse_tag{false};                                   ///< set_vlan() refuses a tag, as a socket does
    std::optional<statusbar::ieee::VlanTag> vlan;             ///< the open port's tag
    size_t receives{0};                                       ///< receive() calls
    std::optional<statusbar::ip::IPv4Address> joined_source;  ///< the last port's source-specific join, if it was one
    bool frame_ports{false};                                  ///< open_*_stream() open frame ports
    uint32_t start_lead{0};                                   ///< a FrameTx's start_lead_frames()
    FrameRx* frame_rx{nullptr};                               ///< the open FrameRx, if any
    FrameTx* frame_tx{nullptr};                               ///< the open FrameTx, if any
    std::vector<ReceivePlan> receive_plans;                   ///< every open_receive_stream()
    std::vector<SendPlan> send_plans;                         ///< every open_send_stream()
};

/// A two-channel L24 stream to `destination`, PT 97, on a direct media clock at `offset` (none: not PTP-locked).
inline auto stream_session(Endpoint const& destination, std::optional<uint32_t> const offset = 0) -> sdp::Session
{  // NOLINT(statusbar-block-statements): a crafted session, field by field
    sdp::Session session{};
    session.origin_address = "192.168.1.10";
    session.connection = sdp::Connection{.address = destination.address, .ttl = 32};
    sdp::Media media{};
    media.port = destination.port;
    media.payload_type = 97;
    media.encoding = sdp::Encoding::l24;
    media.channels = 2;
    if (offset) {
        media.media_clock = sdp::MediaClock{.direct_offset = *offset};
    }
    session.media.push_back(media);
    return session;
}

/// FPP frames of STEREO_L24's pattern from index `timestamp`.
inline auto pattern_payload(uint32_t const timestamp) -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload(size_t{FPP} * 6);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = pattern(timestamp + (i / 6), i % 6);
    }
    return payload;
}

/// An RTP packet of pattern_payload(`timestamp`).
inline auto rtp_packet(uint16_t const seq, uint32_t const timestamp, uint32_t const ssrc = 0x1234, uint8_t const pt = 97)
    -> std::vector<uint8_t>
{
    auto const payload = pattern_payload(timestamp);
    rtp::RtpHeader header{};
    header.init(pt, seq, timestamp, ssrc);
    std::vector<uint8_t> bytes(rtp::RtpHeader::LENGTH + payload.size());
    (void)rtp::serialize_rtp(header, payload, bytes);
    return bytes;
}

/// Whether every big-endian word of `bytes` is `word`.
inline auto all_words_are(std::span<uint8_t const> const bytes, uint32_t const word) -> bool
{
    if (bytes.size() % 4 != 0) {
        return false;
    }
    for (size_t at = 0; at < bytes.size(); at += 4) {
        statusbar::ieee::quadlet_t got{};
        statusbar::span_load(got, bytes.subspan(at, 4));
        if (got.get() != word) {
            return false;
        }
    }
    return true;
}

}  // namespace padl::aes67
