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
- `Destination`, `Grandmaster`, `State`, `Counters` — what a stream is sent to, its ts-refclk, its state and its figures; `DropReason`, `DropCounts` — why a receiver refused packets.
- `Transport`, `ReceivePort`, `SendPort`, `PayloadRenderer` — the transport contract; `UdpTransport`, `XdpTransport` (`XdpTransportOptions`) implement it. `Refusal`, `ReceiveRefusals`, `RefusalCounter` — frames a transport refused before any receiver saw them (`Transport::receive_refusals()`). `SendPort::send_rendered_at()` accepts the packet's departure; `XdpTransport` with `XdpTransportOptions::launch_time` gives every frame a launch time (`LaunchClock`, `set_launch_lead_ns()` / `clear_launch_lead()`), and `settle_launch_time()` puts its queue in or out of launch-time mode.
- `RtcpPort`, `RtcpOptions` — an RTCP session's datagrams beside a stream (`Transport::open_rtcp()`), off the real-time thread.
- `RtcpSession` — `open_sender()`, `open_receiver()`, `on_ready(now)`, `tick(now)`, `close()`, `stats()`, `set_timebase()`; `RtcpSessionOptions` (CNAME, receiver DSCP, a sender's `SenderReports`, the RFC 3550 cadence, `on_sender_report`), `RtcpStats`.
- `SenderReportTime`, `sender_clock_offset()` — a received SR's (RTP timestamp, PTP time) pair, and the RTP offset it gives a sender-clocked stream in a clock domain.
- `RtcpCadence` — when a receiver's next report goes; `Rfc3550Interval` implements RFC 3550 §6.3.1.
- `SendState`, `ReceiveState` — what a sender and a receiver publish for their session.
- `SrClass`, `SrpTSpec`, `srp_frames_per_packet()`, `srp_tspec()`, `multicast_mac_aliases()` — a stream
  under SRP (AES67-2023 Annex C.1.1): the framing of its class's interval, its MSRP TSpec, and
  whether two groups share an Ethernet address. `SendOptions::vlan`, `SendPort::set_vlan()` and
  `Sender::set_vlan()` tag a stream; `XdpTransport::set_receive_vid()` receives tagged ones.
- `Aes67Error` — `enum class`, category `statusbar.aes67`.

## Quick example

```cpp
#include "statusbar/aes67/aes67_receiver.hpp"
#include "statusbar/aes67/aes67_udp_transport.hpp"

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

- `statusbar/aes67/aes67_stream.hpp` — `StreamFormat`, callbacks, `Destination`, `Grandmaster`, `State`, `Counters`.
- `statusbar/aes67/aes67_receiver.hpp` — `Receiver`.
- `statusbar/aes67/aes67_sender.hpp` — `Sender`.
- `statusbar/aes67/aes67_transport.hpp` — `Transport`, `ReceivePort`, `SendPort`, `PayloadRenderer`, `RtcpPort`, `Endpoint`, `Datagram`, `SendOptions`, `RtcpOptions`, `rtcp_endpoint_of()`.
- `statusbar/aes67/aes67_udp_transport.hpp` — `UdpTransport`.
- `statusbar/aes67/aes67_xdp_transport.hpp` — `XdpTransport`, `XdpTransportOptions`.
- `statusbar/aes67/aes67_socket.hpp` — `InterfaceInfo`, `find_interface()`, sockaddr conversion, `destined_for()`, `open_receive_socket()`, `open_send_socket()`, `open_rtcp_socket()`.
- `statusbar/aes67/aes67_rtcp.hpp` — `RtcpSession`, `RtcpSessionOptions`, `RtcpStats`, `RtcpCadence`, `Rfc3550Interval`, `SenderReportTime`, `sender_clock_offset()`.
- `statusbar/aes67/aes67_error.hpp` — `Aes67Error`.

## Dependencies

- **Statusbar modules:** [`mediaclock`](MEDIACLOCK_MODULE.md), [`rtp`](RTP_MODULE.md), [`sdp`](SDP_MODULE.md), `buffer`, `ieee`, `ip`, `itc`, `net` (sockets, `XdpContext`, `CompiledRule`), `status`.
- **System / external:** Linux `recvmmsg()` / `sendmmsg()`, `IP_PKTINFO`; libxdp and an AF_XDP-capable NIC for `XdpTransport`.

## Notes & caveats

### Transport contract

- The control thread (one thread) calls `open_receive()` / `open_send()` and destroys
  ports; these may allocate and make syscalls. The real-time thread calls
  `ReceivePort::receive()`, `SendPort::send_rendered()`, `flush()`,
  `begin_tick()` and `end_tick()`: no allocation, no blocking, no logging.
- Opening and destroying ports is safe while the real-time thread ticks the transport. A
  transport that shares state between ports (XDP) hands port changes to the real-time
  thread through queues, applies them in `begin_tick()`, and frees a closed port's state
  only after the real-time thread has let it go.
- Do not destroy a port while the real-time thread may still call it: swap it out between
  ticks and retire it first. The transport outlives every port it opened (asserted in its
  destructor), and the real-time thread stops ticking it before it is destroyed.
- A datagram's `payload` is the transport's until the next `receive()` on the port or the
  next `begin_tick()`, whichever is first.
- `send_rendered()` reserves the buffer first; when it cannot, `render` is not called. When `render` returns false, or the packet is refused after it
  ran (the XDP TX ring is full), the buffer is released unsent: a batch slot is not
  counted, a UMEM frame goes back to the pool, or, refused by the ring, is freed. A
  transport calls `render` at most once per packet, because the sender pulls its frames
  from the provider inside it.
- `open_rtcp()` and every `RtcpPort` call belong to the control or reactor thread, never
  the real-time one: RTCP is not real-time traffic. A port whose `fd()` is -1 has no
  descriptor that wakes for all its datagrams, and its owner calls `receive()` each
  reactor cycle. The base `Transport` refuses `open_rtcp()` (`operation_not_supported`),
  so test transports need not implement it.
- RTCP DSCP (AES67-2023 Clause 6.2): a sender marks its RTCP as its RTP stream, a receiver
  as it would mark a similar stream. `RtcpOptions::dscp` is that value; the caller passes
  the stream's (or would-be stream's) `SendOptions::dscp`, not the RTCP class of its own.

### Sender

- Packet times (AES67-2023 Clause 7.2.0): a sender sends any whole number of frames per
  packet whose payload fits `MAX_PAYLOAD`, 1440 octets (Clause 6.3); larger fails
  `packet_too_large`. That covers 125 us, 250 us, 333 us, 1 ms and 4 ms (Clauses 7.2.1 and
  7.2.2; 6, 12, 16, 48 and 192 frames at 48 kHz), 4 ms only for few channels (2 of L24 at
  48 kHz). The packet time is fixed per `configure()`.
- Packets lie on a grid of `frames_per_packet`: packet k carries indices
  `[k * fpp + phase, (k + 1) * fpp + phase)`. Its RTP timestamp is
  `PtpMediaClock::rtp_timestamp(first index, 0)`, so the SDP says `mediaclk:direct=0` and
  receivers on the same grandmaster place every sample exactly.
- The send offset gates sending: a packet goes out on the first tick at
  which its last frame's index is `send_offset()` or more behind the current index. A
  network source delivers frames after their index: an AES67 packet `[n, n + 48)` arrives
  no earlier than `(n + 48) / fs` plus its latency. `default_send_offset()` is one 125 µs
  tick, for a provider whose frames are there by their index (generated, captured
  locally, or placed ahead of time); a receiver then sees the packet about a packet time
  after its RTP timestamp (1.125 ms for 1 ms packets at 48 kHz). The lower the send offset, the
  more of a receiver's latency budget is left (a Dante receiver at its 1 ms setting is
  already late with 1 ms packets), but a provider whose frames land later than that has
  them counted lost; one fed from the network (an AES67 receiver's frames) wants about a
  packet plus two ticks. A changed send offset applies from the next
  tick: a longer one waits, a shorter one catches up, up to `MAX_BURST` packets.
- Send phase stagger: with every 1 ms output on phase 0, all of them send on the same
  tick in eight, and that burst, not the mean load, sets the ceiling. Give the kth sender
  `stagger_phase(format, k)`, (k mod 8) ticks modulo the packet (0, 6, ... 42 frames for
  1 ms at 48 kHz), before `configure()`. The phase adds no latency and leaves the
  timestamps on the index grid.
- The SSRC is random per `configure()`, the sequence starts at a random value and counts
  up, and the marker is never set.
- Frames the provider does not have (`Supply::missing`) are sent as silence and counted
  lost; with no provider (`Supply::none`) silence is sent and nothing is counted. Silence
  is the encoding's (`fill_silence()`): zero bytes for L16/L24/L32, and for AM824 the
  word 0x40000000 per sample (MBLA label, zero sample), since a zero word is IEC 60958
  data. A packet that is not all `Supply::frames` is silence throughout. More
  than `MAX_BURST` packets behind, the sender skips ahead and counts the skipped frames
  lost. After a backward step of the clock it sends nothing until now catches up with
  what it has sent, so no index goes out twice, unless the step exceeds `MAX_BURST`
  packets, when it resynchronises.
- Payloads are rendered in place: `tick()` hands the provider the payload span of the
  transport's next TX buffer through `SendPort::send_rendered()`, so the frames are copied
  once, provider to wire buffer. When the transport has no buffer to give (counted
  dropped), the frames are pulled into a staging buffer instead, so a missing pull is
  counted lost all the same; when the transport refuses the packet after rendering, the
  frames were already pulled and the packet is counted dropped without a second pull.
- A sender is in `State::error` while its transport reports `send_error()`, checked every
  tick, and returns to ready when it clears.
- `session()` gives origin the transport's interface address, the group with its TTL, the
  encoding, channels, rate and ptime, `ts-refclk:ptp=IEEE1588-2008:<EUI-64>:<domain>` and
  `clock-domain:PTPv2 <domain>` from `set_grandmaster()` (none set: `ts-refclk:localmac=` with
  `Transport::local_mac()`, the sender's own clock, VSF TR-10-1 §10.4; `references()`), `mediaclk:direct=0`, a source
  filter naming this host for a multicast group, and `a=recvonly`, as AES67's examples and
  Dante write it. A `Grandmaster` whose `version` is `ieee802_1as` is written
  `ts-refclk:ptp=IEEE802.1AS-2011:<EUI-64>` with no domain and no `clock-domain`
  (AES67-2023 Clause 8.2, Example 3); the IPMX Info Block's `ts-refclk` follows it. A
  `traceable` grandmaster (its timeTraceable flag) adds a second, equivalent line,
  `ts-refclk:ptp=<version>:traceable` (RFC 7273 Section 4.8; AES67-2023 Clause 8.2,
  Example 2), so a receiver on another traceable grandmaster can connect too; the Info
  Block keeps the grandmaster's line.
- Session id and version: a receiver (Dante Controller, say) keys a session on the o=
  session id and origin, so a new id each start leaves stale sessions behind. The id is
  stable across restarts: `set_session_id()`, or by default `stable_session_id()`
  (FNV-1a) of the origin address and the name, so two senders on one host with the same
  name need ids of their own. The version starts at the wall clock's seconds (RFC 8866's
  suggestion, so it grows across restarts too) and goes up by one with each
  `configure()`, grandmaster change (`traceable` included) and rename. Ids and versions are kept to 63 bits.
  `continue_from()` carries the id and version (and the send offset, grandmaster, media
  clock and name) to a replacement sender, so its next `configure()` announces the same
  session one version on.
- `set_st2110(true)` gives `session()` ST 2110-30's form (Section 6.2): the channel order as an fmtp
  (`a=fmtp:<pt> channel-order=`, `channel_order()`: the one set, or undefined groups) and `a=maxptime`, the packet
  time. Off by default, so an AES67 receiver sees AES67's attributes alone. A change, and a channel order changed
  while it is on, bumps the session version.
- While an ipmx `RtcpSession` holds the sender (a `Sender::ReportHold` from `hold_reports()`, which the session owns
  while open; the holds are counted, so one session closing lifts no other's), `session()` declares IPMX in
  the fmtp (`a=fmtp:<pt> IPMX`, after any channel order; VSF TR-10-1 §10.1): its SRs then follow §8.10.1. The
  declaration appearing or going bumps the session version. Without a session the SDP is AES67's alone.
- A clock domain other than PTP: the caller ticks the sender with the domain's index
  (`PtpMediaClock::index_at(timebase.to_domain(ptp_now))`, `mediaclock::Timebase`), which
  advances 5 to 7 frames a tick at 48 kHz for a domain within 100s of ppm; `tick()` needs no
  change and sends every packet once, 0, 1 or 2 a tick. `set_sender_clock(true)` makes the
  SDP say `mediaclk:sender` (RFC 7273 §5) instead of `direct=0`, which would claim the RTP
  timestamps are PTP time; `ts-refclk` and `clock-domain` still name the grandmaster, the
  reference the sender reports are in. Such a stream needs its `RtcpSession` with
  `set_timebase()`: the SR is the only statement of its clock.
- Who accepts a `mediaclk:sender` stream: RAVENNA and IPMX receivers that follow its sender
  reports (their asynchronous case), and a `Receiver` with `set_sender_clock(true)`. Dante
  in AES67 mode expects `direct=0` on its PTP and refuses or drifts; nothing beyond the SDP
  telling the truth prevents it listing the announcement.

### Receiver

- `configure()` accepts the media's group and port, payload type, encoding, channels and
  rate, an RFC 4570 source filter if any, and the RFC 7273 direct media clock offset. It
  prefers the offered format with the receiver's encoding (`Media::prefer()`), else the
  primary one; the encoding must be the receiver's (`encoding_mismatch`), and channels and
  rate too (`format_mismatch`). The receiver's ptime need not match the SDP's: each
  packet's frames are counted from its payload (AES67-2023 Clause 7.2.0), up to the SDP's
  ptime, or its maxptime when longer (`max_frames()`); a longer packet fails `size`. A stream without a direct media clock is
  refused (`not_ptp_locked`): its timestamps are not PTP time.
- Where it receives (`receive_endpoint()`, AES67-2023 Clauses 6.1.1 and 8.5.2, RFC 8866
  Clause 5.7): a group's `c=` is joined; a unicast `c=` that is this host's address (a
  transport file, as NMOS IS-05 writes) is bound; a unicast `c=` equal to the origin's
  address that is not this host's is an offer from that sender, received at this host's
  address with the sender as the source filter unless `a=source-filter` names another. A group in 232/8 with a
  source filter that applies to it is joined for that source alone (`Transport::open_receive_from()`, RFC 4607
  Section 4.1): a switch running SSM forwards nothing to an any-source join there. `UdpTransport` joins the socket so,
  `XdpTransport` its kernel membership; another transport opens it any-source and the receiver filters. Any
  other unicast `c=`, 0.0.0.0 among them, fails `foreign_destination`: which addresses are
  this host's the transport decides from the interface's address list
  (`Transport::is_local_address()`, `is_local()`: any 127/8 address on a loopback interface),
  not from a bind the kernel may allow (`ip_nonlocal_bind`). RTCP for a unicast stream
  arrives at the bound address at port + 1 and goes to the sender.
- `set_sender_clock(true)` before `configure()` accepts only `mediaclk:sender` streams (any
  other fails `not_sender_clocked`, `direct` included, as a direct stream would drift in a
  foreign domain). Such a stream has no offset until `set_offset()`: `drain()` learns its
  SSRC, address and sequence (so its session reads only that source's reports) but drops its
  packets (counted) until then, and a new source clears the offset. The offset comes from the stream's first IPMX-form SR:
  `sender_clock_offset(report, timebase, fs)` is the RTP timestamp less the domain's index
  at the PTP time named, rounded to the nearest sample. `set_offset()` is an atomic store,
  safe from the reactor while the real-time thread drains; checking later reports against
  it (a stream not in the domain moves) is the caller's.
- Packets of the stream's payload type are accepted. The first one's SSRC and source
  address are learnt, and another SSRC, or the same SSRC from another address, is dropped
  until the learnt source has been silent for a second; a host copying the SSRC cannot
  reset the sequence. The sequence is
  tracked with `rtp::RtpSequence`; a new source's probation packets are handed on too, and
  only an unconfirmed jump of a validated source is dropped. The payload goes to the
  `FrameHandler` as is.
- Due-tick gate: over a queuing transport (UDP), `drain()` skips the port until a tick
  before the next packet can have been sent (after `[i, i + n)`, the next is sent no
  earlier than index `i + 2n`), then asks every tick until one comes. It never waits more
  than a packet time and a tick, and asks every tick before the first packet, after a
  backlog, and after a clock step back. At 1 ms that is about three receive calls in eight
  ticks instead of eight. The gate relies on the sender's timestamps: a sender stamping
  ahead of capture has its packets read up to a packet time late. Over XDP
  (`drain_every_tick()`), a port keeps datagrams only for the tick they arrived in, so
  there is no gate. `polls()` and `skipped_polls()` count the calls made and skipped.
- Counters: `packets` and `frames` handed on; `lost`, frames in sequence gaps not (yet)
  filled; `late`, frames that arrived out of order (after a later packet), still handed
  on; `dropped`, packets refused, and `drops` the same by `DropReason`: `not_rtp` (no RTP
  header), `payload_type` (not the SDP's), `size` (empty, not whole frames, or more than
  `max_frames()`), `source` (not the source filter's, RFC 4570, or the learnt SSRC from
  another address), `ssrc` (another source while the learnt one is
  live), `sequence` (a duplicate, or a jump RFC 3550 Appendix A.1 has not confirmed) and
  `no_offset` (a sender-clocked stream before `set_offset()`). Checked in that order; a
  packet counts once, under the first reason that refuses it.

### RTCP

- Scope: RFC 3550 §6 as a dedicated device needs it. A sender's session sends, on VSF TR-10-1
  §8.10.1's schedule, one compound packet per report: an SR, then SDES with its CNAME (§6.1 wants the
  CNAME in every compound packet). A receiver's sends an RR with one report block about
  the stream, then SDES. Both send an empty RR, SDES and BYE on `close()` or destruction; a sender's
  session also sends one, at its next `tick()`, for the SSRC it last saw sending once that is gone
  (stopped by `Sender::pause()`, or replaced by a resumed run's, however quickly; `SendState::sent` says whether
  a run has sent, so a clock step that moves `next_index` back onto `first_index` is not a stop), and then none on
  `close()` unless it sent again. The BYE is built on the reactor, never the real-time thread.
  Out of scope: an RFC 3550 wall-clock NTP mode, APP, SDES items past CNAME, RTCP-XR,
  timer reconsideration and member tables (§6.3.3–6.3.7), and RTP/RTCP multiplexing (RFC
  5761).
- A sender's schedule is `RtcpSessionOptions::reports`: `rfc3550` (the default) sends an SR at RFC 3550 §6.3.1's
  randomised interval (Tmin `SENDER_INTERVAL_MS`, 1 s), naming the next packet to send, with no first-packet hold and
  no IPMX declaration; `ipmx` follows TR-10-1 below. Both carry the Info Block, which says how the time field reads.
- The `ipmx` schedule (VSF TR-10-1 §8.10.1): one SR for a run's first packet, then one every N = int(10 ms / ptime)
  packets (`REPORT_SPACING_US`; 10 at 1 ms, 80 at 125 µs), each naming its packet's RTP timestamp and sent before
  that packet and after the one N before. The sender places a run's first packet `FIRST_REPORT_LEAD_US` (20 ms)
  ahead, publishes it (`SendState::first_index`), and sends it when due, whether or not its SR went: audio never
  waits on RTCP. The session sends that SR at its next tick. A clock step back past the packet, or forward past
  a burst, places it afresh, so no SR names a packet never sent. Later SRs go once the previous grid packet has left
  (`SendState::next_index`); `next_wake_ns()` says when, midway through the next SR's window, for the caller's
  timer. One whose packet left first (the caller late) is passed over; a clock step back restarts the grid. A run
  paused before its first packet sent none and gets no BYE. About 100 SRs a second of about
  180 octets: 0.15 Mbit/s against a 9 Mbit/s 8-channel L24 stream.
- One SR form: the time field is the sender's clock in the PTP truncated format (TR-10-1 §8.7: seconds mod 2³²,
  then nanoseconds) and the SR always carries the IPMX Info Block with the PCM Media Info Block. The field is PTP
  time when `ts-refclk` names a PTP grandmaster; under `localmac` it is the sender's free-running clock (§7.1,
  §10.4). AES67-2023 §6.3 asks only for RFC 3550 §6 and prescribes no derivation of the field, and no AES67
  receiver synchronises from it. The pair is exact: the field is the time the SR's packet's first sample is
  sampled (`time_of(index)`), the RTP timestamp that sample's (`mediaclk:direct=0`).
- The packet's time is the sender's own: with no timebase it is written unconverted, so a sender ticked with
  another PTP's index (a member interface's) reports that PTP's time, whatever the reactor's clock reads. A sender
  in another clock domain (`set_timebase()`, read per report): the SR maps the packet's domain time to PTP time
  through the timebase (`from_domain()`, within a nanosecond), and none is sent while the timebase is not running.
  The identity timebase gives the same bytes as none; the SR path costs one seqlock load per report.
- A receiver's session records the pair from each of the stream's SRs that carries the IPMX
  Info Block (`RtcpStats::remote_sr`, `RtcpSessionOptions::on_sender_report`); an SR
  without it gives no pair (its field may be RFC 3550's wall clock). Whether the pair's time is the receiver's
  PTP is the sender's `ts-refclk`'s to say, which the handler checks.
- The Info Block's strings are the SDP's (the first of `Sender::references()`, `ptp=` or `localmac=`;
  `media_clock_value()` of `Sender::media_clock()`); `measuredsamplerate` is the
  nominal rate in PTP time, `round(fs * (1 + r))` in a domain of rate offset r, kept
  until the rate moves a whole hertz from it, a change of it alone one version on (the
  count is the session's; a replacement sender has a new SSRC); the channel order is `Sender::set_channel_order()`, by default undefined
  groups of at most 64 (ST 2110-30's U01–U64), `SMPTE2110.(U02)` or
  `SMPTE2110.(U64,U64,U08)`; the 8-bit channel count is clamped at 255; its version is `Sender::info_block_version()`, one on with every
  `configure()`, grandmaster and channel-order change. TR-10-3 allows sample sizes of
  16 and 24; an L32 stream says 32.
- DSCP (AES67-2023 Clause 6.2): a sender's session marks its reports with its stream's DSCP
  (`Sender::set_dscp()`); a receiver's with `RtcpSessionOptions::dscp`, the DSCP it would
  send a similar stream with. Table 1's defaults are AF41 (34) for media and EF (46) for
  clock; `DSCP_MEDIA` is 34, and a caller on a network with another policy sets its own
  (Dante marks media EF, 46).
- A receiver's interval keeps RFC 3550 §6.2's 5 s fixed minimum; an `rfc3550` sender's a chosen 1 s. `Rfc3550Interval` applies §6.3.1 with two
  members and one sender over 5 % of the stream's bandwidth (the share never binds at AES67 rates), halves the
  first interval, and randomises each. Receivers reporting are not counted into it.
- Round trip: an RR echoes the middle bits of the SR's field, which for the PTP form are
  not RFC 3550's 1/65536 s units, so the sender keeps the PTP time of its last
  `SR_HISTORY` SRs (128, 1.28 s of them) and finds the one an RR's LSR names; the RTT is arrival − that time −
  DLSR, resolved to DLSR's 15.26 µs.
- Jitter (RFC 3550 §6.4.1, Appendix A.8) is computed in `Receiver::accept()` from the
  drain's index as the arrival time, so it is quantised to one tick (125 µs, 6 samples at
  48 kHz), and only the first packet of each drain counts: packets drained together share
  an arrival time, and their timestamp gaps would read as jitter. A diagnostic, not a
  measurement. Loss figures come from the receiver's
  `RtpSequence`; the session forms the interval's fraction from the cumulative
  `expected` and `received` it last reported, so nothing on the real-time thread resets.
- A session receives its own reports when the group loops them back and skips its SSRC.
  A receiver's session reads RTCP only from `ReceiveState::source`: the address the
  stream's packets come from, or before the first one the SDP's source filter; with
  neither it reads nothing, and after the first packet only the stream's SSRC counts. Its
  SRs drive a sender-clocked stream's offset, so no other host may send them. A sender's
  "remote" is whichever receiver reported on its stream last.
- The CNAME is the caller's: RFC 3550 §6.5.1's `user@host` shape, as
  `<identity>@<address>`, at most 255 bytes.
- `Receiver::rtcp_options()` reads the endpoint from the SDP: `a=rtcp:` when given,
  else the media's address at port + 1. An `a=rtcp:` unicast address is where reports go
  (RFC 3605), received here at the same port; otherwise, for a unicast stream, reports go
  to the sender (the source filter's source, else the origin address) at the same port. `a=rtcp-mux`
  leaves the stream with no RTCP (`rtcp_multiplexed`).
- Times are PTP nanoseconds supplied by the caller (`on_ready()`, `tick()`); a report
  scheduled far beyond the minimum (PTP stepped back) is re-armed.

### UdpTransport

- The one transport that carries unicast (AES67-2023 Clause 6.1.1), both ways: every
  stream has its own socket, a group's joined and a unicast address's bound, so groups and
  unicast addresses never share one. `XdpTransport` refuses unicast.
- A receiver's socket is bound to its destination address itself, so a group's socket
  sees only that group and a unicast receiver's specific bind wins its address:port over
  the group sockets sharing the port; `IP_PKTINFO` checks each datagram's destination as
  well. One unicast receiver per address and port.
- Group sockets, RTP and RTCP alike (`open_receive_socket()`), are scoped to the
  transport's interface, so two transports on two NICs can receive the same group. Linux
  delivers a group's datagrams to every socket bound to it, whichever interface joined
  it, unless `IP_MULTICAST_ALL` is 0; with it 0, a socket gets only the groups it joined,
  on the interfaces it joined them on. Each socket sets it to 0, and `receive()` also
  drops a datagram whose `IP_PKTINFO` `ipi_ifindex` is not the transport's interface (one
  compare on the ancillary data already read). `SO_BINDTODEVICE` would scope the socket
  too, but needs `CAP_NET_RAW` before Linux 5.7 and also pins the socket's sends.
  `aes67_veth_test` proves it on two veth pairs; it needs root, or runs unprivileged in a
  user and network namespace:
  `unshare -rn sh -c 'ip link set lo up; build/statusbar_test statusbar/aes67/aes67_veth_test'`.
- `receive_refusals()` counts a truncated datagram (longer than `MAX_DATAGRAM`) as
  `malformed`; one `IP_PKTINFO` puts on another address or interface (another group's on a
  shared port) is skipped uncounted, and the receiver checks the rest (`Counters::drops`).
- A sender's socket is unconnected and every message names the destination, so a unicast
  receiver's ICMP port-unreachable never fails a send (`open_send_socket()`). Multicast goes
  out of the interface with the configured TTL; unicast is bound to the interface's address
  and leaves by it (`net::set_unicast_send()`: `IP_UNICAST_IF`, unprivileged, unlike
  `SO_BINDTODEVICE`), whatever the routing table prefers, with `SendOptions::ttl` and socket
  priority 0, so the DSCP's priority (Linux's `rt_tos2priority()`: AF41 is 2) never puts it
  in an SR class's queue (AES67-2023 Annex C.1.2). Both carry the sender's DSCP (`SendOptions::dscp`, from
  `Sender::set_dscp()`, by default `rtp::FrameAddressing::DSCP_MEDIA`, 34). A tick's packets are written straight
  into the `SEND_BATCH` slots of the sender's `sendmmsg()` batch.
- An RTCP port is one socket for both directions. For a group it is bound to the group at
  the RTCP port, joined, and sends there with loopback on, so a session receives its own
  reports and must skip its own SSRC; for a unicast peer it is bound to `RtcpOptions::local`
  (by default this address at the destination port), without `SO_REUSEADDR`, leaves as its
  stream's socket does (by the interface, with `RtcpOptions::ttl`, at socket priority 0),
  and sends to the peer: a second port on the same unicast address and port is refused
  (`address_in_use`) rather than splitting the reports between the two.

### XdpTransport

- The XDP program redirects to the socket the untagged IPv4 UDP frames, options or none
  (the rules pin the version, not the IHL), whose destination is exactly one of the joined groups (the rule matches the group's MAC and
  the address's first two octets, which together are the whole address) and passes
  everything else to the kernel. With `set_receive_vid(vid)` one more rule, after the
  groups' (so setting or clearing the VID rewrites only the tail), redirects every IPv4
  group's UDP frames tagged with `vid`, any PCP (`tagged_groups_rule()`: MAC 01:00:5e with
  bit 24 clear, RFC 1112 Section 6.4); the ports accept their groups' and count the rest
  `unmatched`, and the kernel no longer sees tagged multicast on that VID. `begin_tick()`
  drains the RX ring once and hands each frame, parsed with `rtp::parse_rtp_frame()`, to the ports for its destination group and
  port; senders write whole frames into UMEM and `end_tick()` kicks the NIC.
- Group membership is still the kernel's: each receive port holds an unbound UDP socket
  joined to its group (`net::open_group_membership()`), so IGMP reports go out and the
  NIC's multicast filter keeps the group. Nothing reads it, and the tick makes no system
  call for receive ports beyond the RX ring. What the program passes is not received: a
  priority-tagged frame (VID 0, see Limits), a frame tagged with a VID other than
  `set_receive_vid()`'s, or a frame on a queue other than `queue_id()` (RSS below).
- `begin_tick()` is the only place the port list and the installed rules change: it adopts
  new ports from a bounded queue, drops closed ones (handing their state back on a retire
  queue, freed on the control thread's next `open_*()` or port destruction, or by the
  destructor), and reinstalls the rules when the set changed. That is a BPF map update, a
  syscall on the real-time thread, only when receivers come or go.
- TX pool: senders keep `TX_POOL` UMEM frames back, topped up each tick before the fill
  ring is; those are never offered to the kernel. A send claims a frame from the pool,
  writes it and commits it to the TX ring; a frame that does not fit, or whose render is
  abandoned, goes back to the pool (never to the kernel), and one the full TX ring refuses
  is freed to the UMEM. The rest of the UMEM backs the fill ring (`FILL_RING` entries) and
  the frames in flight.
- UMEM frames are `net::XdpConfig`'s default 4096 octets. The kernel keeps 256 octets of XDP
  headroom in each, and igb sizes its receive buffers in 1 KiB units below what is left:
  2048-octet frames give 1 KiB buffers, and igb hands the socket a longer frame cut to its
  first KiB, which `parse_udp_frame()` refuses (an 8-channel L24 1 ms packet is 1206
  octets). 4096 leaves 3 KiB buffers. Check a NIC's queue with `ethtool -d` (SRRCTL). The
  UMEM is then 8 MiB locked (`UMEM_FRAMES` × 4096), a default `RLIMIT_MEMLOCK`'s whole 8 MiB:
  the process needs `CAP_IPC_LOCK` or a raised limit.
- Refused kicks: each `end_tick()` kick the kernel refuses is counted with its errno
  (`tx_flush_failures()`, `tx_flush_error()`); `TX_FAILING_AFTER` (8, 1 ms of ticks) in a
  row set `send_error()`, which every sender reports as its error, until a kick goes
  through.
- Counters: `socket_stats()` is the socket's `net::XdpSocketStats` as of the last
  `end_tick()` (frames received and sent, ring-full, UMEM-exhausted, kicks made, skipped
  and transient), and `tx_pool_empty_count()` the sends that found the TX pool empty;
  both may be read from any thread.
- Refusals: `receive_refusals()` counts the redirected frames no receive port kept, by
  `Refusal`: `malformed` (`parse_udp_frame()` refused it: cut short, as a NIC buffer
  smaller than the frame leaves it, or lengths past its end), `unmatched` (a joined
  group's frame to a port no receive or RTCP port has, such as RTCP while no session
  runs), `not_rtp` (to a receive port, but not RTP) and `port_full` (every port for it already
  held `PORT_BATCH` datagrams this tick). A frame counts once, and only when no port kept it;
  the stream's own RTCP on its RTP port (`is_multiplexed_rtcp()`, RFC 5761 Section 4) is
  ignored uncounted, by the transport and by the receiver alike. Relaxed atomics, written by
  `begin_tick()`, read from any thread. `dispatch_frame()` is that per-frame step.
- Limits: multicast destinations only, both ways (a unicast receiver would need a rule
  stealing the host's UDP, a unicast sender ARP): unicast is `UdpTransport`'s alone, and
  nothing on the AF_XDP path tests for it; at most `MAX_GROUPS` distinct groups;
  `MAX_RULES` rules, the groups' and the tagged one, as far as core's TCAM
  (`net::tcam_default_capacity`) holds them: the tagged rule keeps its slot, and a group
  past the room left is dropped and reported by `rules_status()`; a joined group's frames to ports no
  receiver here listens on do not reach the kernel (the rule cannot see the UDP port), so
  other listeners on the same group miss them; tagged frames with one VID only; one queue
  (`Options::queue`, by default `QUEUE`, 0), so the NIC must deliver the groups there, and a NIC that strips VLAN tags
  (`rxvlan`) delivers them untagged.
- Priority-tagged frames (VID 0, untagged by IEEE 802.1Q) are not received, by design: with
  the tag the destination address lies past the 32-octet window, so no rule can classify
  them by group, and no sender known on AES67 networks sends them. A TCAM of 64 rules (a
  second rule per group, keyed on the MAC) is the path if one appears.
- RSS: a NIC with several RX queues hashes UDP flows over all of them, so `open()` always
  steers every RSS bucket to its queue (`net::set_rss_indirection_all`, `CAP_NET_ADMIN`), and
  fails with `Aes67Error::rss_not_steered` when it cannot (`rss_verdict()`): a spread table
  would lose groups silently. It steers before the program attaches (on igb an attach resets
  the adapter); the write is one ioctl, so a refusal leaves the table as it was, and a socket
  that then fails to open puts the table back. `steer_rss()` is that step alone. A queue other
  than 0 (an SR class queue a credit-based shaper paces) receives the same steering; traffic the
  NIC does not hash (non-IP) stays on its default queue and the kernel.
  A NIC without RSS reports `operation_not_supported` in `rss_status()` and opens; it needs one
  combined channel or a flow-steering rule. The table is left steered at close. `UdpTransport`
  never touches RSS: the kernel's sockets receive on every queue.
- RTCP: the program steals every frame to a redirected group, whatever its port, so an
  RTCP port's kernel socket would miss reports to a group a receiver here has joined.
  `begin_tick()` therefore parses each frame as UDP (`rtp::parse_udp_frame()`; the RTP
  parse is only for receive ports) and copies one for an open RTCP port's group and port
  into that port's `RtcpPipe` (`DEPTH` datagrams of at most `MAX_DATAGRAM`, 1500, bytes,
  as a kernel socket keeps; the rest are dropped and counted in `RtcpPort::dropped()`),
  and the frame is not held. `receive()`
  drains the pipe and the kernel socket, which gets the group's reports while no rule
  redirects it and sends every report (the program steals RX only). RTCP ports do not
  add rules. `fd()` is -1.
- AF_XDP zero-copy, attach and pinning behaviour is hardware-dependent and verified on the
  bench box, not in unit tests; `aes67_xdp_test` covers the frame, pool, health and launch-time logic.

### Streams under SRP

- AES67-2023 Annex C.1.1 carries a stream as an IEEE 802.1Q Clause 35 reservation. `srp_tspec(format,
  class)` is its TSpec (Clause 35.2.2.8.4): MaxFrameSize counts the IPv4, UDP and RTP headers, the
  payload and one spare octet, no Ethernet overhead (Annex C.1.1.1); MaxIntervalFrames the packets
  that can start in one class interval (125 us for class A, 250 us for B, Clause 34.4).
- A packet time longer than the interval would reserve a whole frame every interval for one sent
  each packet time. `srp_frames_per_packet(rate, class)` is the fewest frames whose packet time
  reaches the interval, AES67-2023 Table 2's "125 microseconds" in class A (6 at 48 kHz, exactly
  125 us; 6 at 44.1 kHz, 136 us, one frame per interval; 12 at 96 kHz) and "250 microseconds" in
  class B (12, 12, 24), for which MaxIntervalFrames is 1. Which packet times a caller sends or
  accepts under SRP is its own policy.
- A tagged stream: `SendOptions::vlan` opens a port tagged; `SendPort::set_vlan()` switches it between
  tagged and untagged from the next frame (`XdpTransport`: one atomic word, wait-free for the
  tick, read per frame), and the sender's run, sequence and timestamps go on. `UdpTransport` cannot tag and
  refuses a tag. `Sender::set_vlan()` keeps the tag for its next `configure()`.
- Reservations filter by destination MAC (Clause 35.2.2.8.3): `multicast_mac_aliases()` says when two
  groups share one.

### Launch time

- A sender hands each packet over with its departure (`SendPort::send_rendered_at`): the time of
  the tick that sends it, as the caller gives `Sender::tick(now_index, tick_ns)`, in the ns the
  sending NIC's PHC reads (PTP time; a member interface's domain time). The sender reconstructs
  nothing: a tick without a time sends untimed, a stalled sender's burst departs at the tick that
  catches up. The RTP timestamp is the frames' index (RFC 3550 Section 5.1, capture time) and does
  not move with the departure or the lead.
- `XdpTransport` opened with `XdpTransportOptions::launch_time` (the host found that the driver
  honours it, `net::netdev::launch_time_supported`; a build, kernel or NIC without it opens without
  and records `launch_time_error()`) gives every frame on the socket a launch time (`LaunchClock`),
  in the NIC's PHC time: now (`net::phc_now_ns()` of the clock the host gives `set_phc_clock()`;
  without one, 0: untimed, so every frame goes at once and a lead has no effect) until
  `set_launch_lead_ns()` puts a lead in force, then departure plus lead, never earlier than the last
  time committed (`net::LaunchOrder`: igc's queue is a FIFO; the last time a second or more ahead of
  now is a clock step; a time already past at commit, an overrunning tick, goes at once and is
  counted, as are holds and steps, in `launch_clock().order()`); `clear_launch_lead()` is now again.
  An untimed send (a tick without a time) goes at now. RTCP and SAP go by kernel sockets,
  never through the socket.
- The lead moves the wire time only: the packet leaves at departure plus lead, free of the sending
  CPU's jitter, while its timestamp and the receiver's placement are unchanged, so launch time
  changes no latency; it lets a receiver's link offset cover the lead plus the network transit
  instead of the sender's jitter as well (AES67-2023 Clause 7.4). A lead of 0 would be past when
  the tick commits the frame (the NIC sends at once, as without launch time), so the lead is a
  whole tick or more.
- The NIC honours the time only with the socket's queue in launch-time mode:
  `XdpTransport::settle_launch_time(interface, mode)` lays core's mqprio root
  (`LAUNCH_TIME_QDISC_MAJOR`, every priority mapped away from `QUEUE`) with an offloaded etf on the
  socket's queue (`net::tc::settle_launch_time_queue`). On igc the first etf and the removal of the
  last each reset the adapter (its PHC time is kept across the reset, `igc_ptp_time_restore`), so a
  host settles it once, before anything streams, and pauses launch time by clearing the lead, not by
  removing the etf. Stock igc estimates its cycle from CLOCK_TAI, so on it that holds only while the
  system clock follows the PHC. `unsupported` (igb) looks nothing up and sends nothing. None of this
  has run on an igc yet.
- `launch_time_queue(tx_queues)` also puts PTP event traffic on `PTP_QUEUE` (2) with 4 TX queues or
  more (`net::tc::PTP_PRIORITY`, ptp4l's `socket_priority 7`), queue 1 idle and the rest on queue 3,
  as an SR shaper lays them, and keeps its root without launch time: `off` lays the root (a host
  without launch time calls it before the socket opens) and removes only the etf. The order
  assumes the lowest queue has the highest priority, as on igb and igc (I210 Datasheet Section
  7.2.7.4), the drivers with AF_XDP launch time; an mqprio root of another handle goes first.

## Further reading

- AES67-2023 — interoperability requirements for audio over IP (§6.2 DSCP, §6.3 transport and RTCP).
- RFC 3550 — RTP and RTCP; RFC 3605 — the RTCP attribute; RFC 7273 — clock source signalling; RFC 4570 — source filters.
- VSF TR-10-1:2024 §8.7, §8.10.1, §10.1 and §10.4, and TR-10-3:2024 §11 — the IPMX SR, its schedule, the SDP
  declaration and `localmac`, and the Info Block.
- [`mediaclock`](MEDIACLOCK_MODULE.md), [`rtp`](RTP_MODULE.md), [`sdp`](SDP_MODULE.md), [`sap`](SAP_MODULE.md).
- statusbar-core `net` module — `XdpContext`, `CompiledRule`, sockets.
