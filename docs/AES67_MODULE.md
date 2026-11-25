[← back to module index](README.md)

# aes67

AES67 streams: `Receiver` and `Sender`, each one RTP stream described by SDP with frames
addressed by PTP sample index, over a `Transport`: `UdpTransport` (sockets, any host) or
`XdpTransport` (AF_XDP raw frames on one NIC queue); and `RtcpSession`, the RTCP beside a
stream.

## Overview

A `Receiver` is configured from an SDP media section and, each tick, drains what its
transport received, validates source and sequence, turns each RTP timestamp into a
sample index with `mediaclock::PtpMediaClock`, and hands the payload to its
`FrameHandler`. A `Sender` asks its `FrameProvider` for the frames of every packet due on
the sample-index grid, renders them straight into the transport's transmit buffer, and
describes its stream in SDP (`session()`) for SAP and control.

Both run on one real-time thread that ticks every 125 µs, bracketed by the transport's
`begin_tick()` (before any receiver drains) and `end_tick()` (after every sender has
sent). Configuration happens on a control thread.

An `RtcpSession` runs on the reactor (the control thread in a daemon), beside a configured
`Sender` or `Receiver`. The stream publishes what RTCP needs through a seqlock each tick
(`SendState`, `ReceiveState`); the session reads it, forms its SR or RR from it and the PTP
time it is given, sends it through an `RtcpPort`, and parses what arrives.

## Key types

- `StreamFormat` — encoding, channels, rate and frames per packet (the ptime).
- `FrameHandler` / `FrameProvider` — the real-time callbacks, `sg14::inplace_function`s with 64 bytes of capture; `Supply` is what a provider gave.
- `receive_endpoint()` — where a stream described by an SDP arrives here: its group, this host's address, or, for an offer whose `c=` is its origin, this host's address.
- `Receiver` — `configure(session, media_index)`, `drain(now_index)`, counters, state, learnt SSRC; `set_sender_clock()` and `set_offset()` for a stream on the sender's clock.
- `Sender` — `configure(destination | session)`, `tick(now_index)`, `pause()` (idle until the next tick, which starts a new run on the index grid under a new SSRC and sequence, RFC 3550 Section 8; the RTCP session says BYE for the old SSRC at its next tick and sends no SR meanwhile), `session()`, send offset, packet phase, session id, grandmaster, `set_sender_clock()`, name, `continue_from()`. `tick(now_index, tick_ns)` hands each packet to the transport departing at the tick's time.
- `Depacketizer` — a receiver's per-packet rules: admission (payload type, size, source filter, SSRC learning, RFC 3550 Appendix A.1 sequence check), the reorder window, the interarrival jitter and the counters. `Receiver` runs one on its packet path; a frame port runs one beside the network.
- `PacketSchedule` — a sender's packet grid, due rule, resynchronisation, held start and each run's SSRC and sequence. `Sender` runs one; a frame port's engine runs one from the `SendRun` it is given.
- `ReceivePlan`, `SendPlan`, `SendRun`, `receive_plan()` — what a stream is, as a transport is asked to carry it (`Transport::open_receive_stream()` / `open_send_stream()`, by default today's `open_receive()` / `open_receive_from()` / `open_send()`), its PTP grandmaster and domain when known, and where a sender's run starts.
- `FrameReceive`, `FrameSend`, `FrameDrain`, `SendProgress` — frame ports (`ReceivePort::frames()`, `SendPort::frames()`): ports that packetise and depacketise themselves and accept or give frames at sample indices; `Receiver::drain()` and `Sender::tick()` use that path when a port is one.
- `Destination`, `Grandmaster`, `named_grandmaster()`, `State`, `Counters` — what a stream is sent to, its ts-refclk and the grandmaster an SDP's ts-refclk names, its state and its figures; `DropReason`, `DropCounts` — why a receiver refused packets.
- `Transport`, `ReceivePort`, `SendPort`, `PayloadRenderer` — the transport contract; `UdpTransport`, `XdpTransport` (`XdpTransportOptions`) implement it. `Refusal`, `ReceiveRefusals`, `RefusalCounter` — frames a transport refused before any receiver saw them (`Transport::receive_refusals()`). `SendPort::send_rendered_at()` accepts the packet's departure; `XdpTransport` with `XdpTransportOptions::launch_time` gives every frame a launch time (`LaunchClock`, `set_launch_lead_ns()` / `clear_launch_lead()`), and `settle_launch_time()` puts its queue in or out of launch-time mode.
- `RtcpPort`, `RtcpOptions` — an RTCP session's datagrams beside a stream (`Transport::open_rtcp()`), off the real-time thread.
- `RtcpSession` — `open_sender()`, `open_receiver()`, `on_ready(now)`, `tick(now)`, `close()`, `stats()`, `set_timebase()`; `RtcpSessionOptions` (CNAME, receiver DSCP, a sender's `SenderReports`, the RFC 3550 cadence, `on_sender_report`), `RtcpStats`.
- `SenderReportTime`, `sender_clock_offset()` — a received SR's (RTP timestamp, PTP time) pair, and the RTP offset it gives a sender-clocked stream in a clock domain.
- `RtcpCadence` — when a receiver's next report goes; `Rfc3550Interval` implements RFC 3550 Section 6.3.1.
- `SendState`, `ReceiveState` — what a sender and a receiver publish for their session.
- `SrClass`, `SrpTSpec`, `srp_frames_per_packet()`, `srp_tspec()`, `multicast_mac_aliases()` — a stream
  under SRP (AES67-2023 Annex C.1.1): the framing of its class's interval, its MSRP TSpec, and
  whether two groups share an Ethernet address. `SendOptions::vlan`, `SendPort::set_vlan()` and
  `Sender::set_vlan()` tag a stream; `XdpTransport::set_receive_vid()` receives tagged ones.
- `Aes67Error` — `enum class`, category `padl.aes67`.

## Quick example

```cpp
#include "padl/aes67/aes67_receiver.hpp"
#include "padl/aes67/aes67_udp_transport.hpp"

using namespace statusbar;

auto receive(std::string_view interface, sdp::Session const& session) -> Status
{
    auto transport = aes67::UdpTransport::open(interface);
    if (!transport) {
        return forward_failure(transport);
    }
    aes67::StreamFormat const format{.encoding = sdp::Encoding::l24, .channels = 2};
    aes67::Receiver receiver{format, [](uint64_t, uint32_t, std::span<uint8_t const>) {}, **transport};
    if (auto configured = receiver.configure(session); !configured) {
        return configured;
    }
    // On the real-time thread, every 125 us tick:
    //   (*transport)->begin_tick(); receiver.drain(now_index); (*transport)->end_tick();
    return success();
}
```

## Headers

- `padl/aes67/aes67.hpp` — the module header: every header below.
- `padl/aes67/aes67_stream.hpp` — `StreamFormat`, callbacks, `Destination`, `Grandmaster`, `State`, `Counters`.
- `padl/aes67/aes67_receiver.hpp` — `Receiver`, `receive_endpoint()`, `receive_plan()`.
- `padl/aes67/aes67_sender.hpp` — `Sender`.
- `padl/aes67/aes67_depacketizer.hpp` — `Depacketizer`.
- `padl/aes67/aes67_packet_schedule.hpp` — `PacketSchedule`.
- `padl/aes67/aes67_plan.hpp` — `ReceivePlan`, `SendPlan`, `SendRun`.
- `padl/aes67/aes67_transport.hpp` — `Transport`, `ReceivePort`, `SendPort`, `FrameReceive`, `FrameSend`, `FrameDrain`, `SendProgress`, `PayloadRenderer`, `RtcpPort`, `Endpoint`, `Datagram`, `SendOptions`, `RtcpOptions`, `rtcp_endpoint_for()`.
- `padl/aes67/aes67_udp_transport.hpp` — `UdpTransport`.
- `padl/aes67/aes67_xdp_transport.hpp` — `XdpTransport`, `XdpTransportOptions`.
- `padl/aes67/aes67_socket.hpp` — `InterfaceInfo`, `find_interface()`, `PKTINFO_SPACE`, `open_receive_socket()`, `open_send_socket()`, `open_rtcp_socket()`.
- `padl/aes67/aes67_rtcp.hpp` — `RtcpSession`, `RtcpSessionOptions`, `RtcpStats`, `RtcpCadence`, `Rfc3550Interval`, `SenderReportTime`, `sender_clock_offset()`.
- `padl/aes67/aes67_error.hpp` — `Aes67Error`.
- `padl/aes67/aes67_test_support.hpp` — `LoopTransport` (and its frame ports, `FrameRx` and `FrameTx`, under `frame_ports`), `PatternSource`, `RecordingSink`, crafted sessions and packets, for tests.

## Dependencies

- **Statusbar modules:** [`mediaclock`](MEDIACLOCK_MODULE.md), [`rtp`](RTP_MODULE.md), [`sdp`](SDP_MODULE.md), `buffer`, `ieee`, `ip`, `itc`, `net` (sockets, `XdpContext`, `CompiledRule`), `status`.
- **System / external:** Linux `recvmmsg()` / `sendmmsg()`, `IP_PKTINFO`; libxdp and an AF_XDP-capable NIC for `XdpTransport`.

## Notes & caveats

- Threads: one control thread opens and destroys ports, the real-time thread drains, sends and
  ticks the transport without allocating, blocking or logging, and `RtcpSession` runs on the
  reactor. The full transport contract, `UdpTransport` (the one transport carrying unicast)
  and streams under SRP are in [AES67_TRANSPORTS.md](AES67_TRANSPORTS.md).
- A sender's packet times, index grid, send offset and phase stagger, silence for missing
  frames, its SDP and session id, and clock domains: [AES67_SENDER.md](AES67_SENDER.md).
- What a receiver accepts from SDP, where it receives, sender-clocked streams, the learnt SSRC
  and source, the due-tick gate and its counters: [AES67_RECEIVER.md](AES67_RECEIVER.md).
- RTCP's scope, the `rfc3550` and `ipmx` sender schedules, the IPMX sender report, intervals,
  jitter and the CNAME: [AES67_RTCP.md](AES67_RTCP.md).
- `XdpTransport`'s rules, group membership, queues and RSS, and launch time:
  [AES67_XDP.md](AES67_XDP.md).

## Further reading

- AES67-2023 — interoperability requirements for audio over IP (Clause 6.2 DSCP, Clause 6.3 transport and RTCP).
- RFC 3550 — RTP and RTCP; RFC 3605 — the RTCP attribute; RFC 7273 — clock source signalling; RFC 4570 — source filters.
- VSF TR-10-1:2024 Sections 8.7, 8.10.1, 10.1 and 10.4, and TR-10-3:2024 Section 11 — the IPMX SR,
  its schedule, the SDP declaration and `localmac`, and the Info Block.
- [AES67_SENDER.md](AES67_SENDER.md), [AES67_RECEIVER.md](AES67_RECEIVER.md),
  [AES67_RTCP.md](AES67_RTCP.md), [AES67_TRANSPORTS.md](AES67_TRANSPORTS.md),
  [AES67_XDP.md](AES67_XDP.md) — this module's topic guides.
- [`mediaclock`](MEDIACLOCK_MODULE.md), [`rtp`](RTP_MODULE.md), [`sdp`](SDP_MODULE.md), [`sap`](SAP_MODULE.md).
- statusbar-core `net` module — `XdpContext`, `CompiledRule`, sockets.
