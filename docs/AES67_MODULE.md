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
- `Receiver` — `configure(session, media_index)`, `drain(now_index)`, counters, state, learnt SSRC; `set_sender_clock()` and `set_offset()` for a stream on the sender's clock.
- `Sender` — `configure(destination | session)`, `tick(now_index)`, `pause()` (idle until the next tick, which starts a new run on the index grid under a new SSRC and sequence, RFC 3550 Section 8; the RTCP session says BYE for the old SSRC at its next tick and sends no SR meanwhile), `session()`, send offset, packet phase, session id, grandmaster, `set_sender_clock()`, name, `continue_from()`. `tick(now_index, tick_ns)` hands each packet to the transport departing at the tick's time.
- `Destination`, `Grandmaster`, `State`, `Counters` — what a stream is sent to, its ts-refclk, its state and its figures; `DropReason`, `DropCounts` — why a receiver refused packets.
- `Transport`, `ReceivePort`, `SendPort`, `PayloadRenderer` — the transport contract; `UdpTransport`, `XdpTransport` (`XdpTransportOptions`) implement it. `Refusal`, `ReceiveRefusals`, `RefusalCounter` — frames a transport refused before any receiver saw them (`Transport::receive_refusals()`). `SendPort::send_rendered_at()` takes the packet's departure; `XdpTransport` with `XdpTransportOptions::launch_time` gives every frame a launch time (`LaunchClock`, `set_launch_lead_ns()` / `clear_launch_lead()`), and `settle_launch_time()` puts its queue in or out of launch-time mode.
- `RtcpPort`, `RtcpOptions` — an RTCP session's datagrams beside a stream (`Transport::open_rtcp()`), off the real-time thread.
- `RtcpSession` — `open_sender()`, `open_receiver()`, `on_ready(now)`, `tick(now)`, `close()`, `stats()`, `set_timebase()`; `RtcpSessionOptions` (CNAME, SR interval, receiver DSCP, cadence, `on_sender_report`), `RtcpStats`.
- `SenderReportTime`, `sender_clock_offset()` — a received SR's (RTP timestamp, PTP time) pair, and the RTP offset it gives a sender-clocked stream in a clock domain.
- `RtcpCadence` — when the next report goes; `Rfc3550Interval` implements RFC 3550 §6.3.1.
- `SendState`, `ReceiveState` — what a sender and a receiver publish for their session.
- `SrClass`, `SrpTSpec`, `srp_format()`, `srp_tspec()`, `multicast_mac_aliases()` — a stream
  under SRP (AES67-2018 Annex C.1.1): its framing cut to the class interval, its MSRP TSpec, and
  whether two groups share an Ethernet address.
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
- `statusbar/aes67/aes67_sender.hpp` — `Sender`, `parse_grandmaster()`.
- `statusbar/aes67/aes67_transport.hpp` — `Transport`, `ReceivePort`, `SendPort`, `PayloadRenderer`, `RtcpPort`, `Endpoint`, `Datagram`, `SendOptions`, `RtcpOptions`, `rtcp_endpoint_of()`.
- `statusbar/aes67/aes67_udp_transport.hpp` — `UdpTransport`.
- `statusbar/aes67/aes67_xdp_transport.hpp` — `XdpTransport`, `XdpTransportOptions`.
- `statusbar/aes67/aes67_socket.hpp` — `InterfaceInfo`, `find_interface()`, sockaddr conversion, `destined_for()`, `open_receive_socket()`, `open_rtcp_socket()`.
- `statusbar/aes67/aes67_rtcp.hpp` — `RtcpSession`, `RtcpSessionOptions`, `RtcpStats`, `RtcpCadence`, `Rfc3550Interval`, `SenderReportTime`, `sender_clock_offset()`.
- `statusbar/aes67/aes67_error.hpp` — `Aes67Error`.

## Dependencies

- **Statusbar modules:** [`mediaclock`](MEDIACLOCK_MODULE.md), [`rtp`](RTP_MODULE.md), [`sdp`](SDP_MODULE.md), `buffer`, `ieee`, `ip`, `itc`, `net` (sockets, `XdpContext`, `CompiledRule`), `status`.
- **System / external:** Linux `recvmmsg()` / `sendmmsg()`, `IP_PKTINFO`; libxdp and an AF_XDP-capable NIC for `XdpTransport`.

## Notes & caveats

### Transport contract

- The control thread (one thread) calls `open_receive()` / `open_send()` and destroys
  ports; these may allocate and make syscalls. The real-time thread calls
  `ReceivePort::receive()`, `SendPort::send()`, `send_rendered()`, `flush()`,
  `begin_tick()` and `end_tick()`: no allocation, no blocking, no logging.
- Opening and destroying ports is safe while the real-time thread ticks the transport. A
  transport that shares state between ports (XDP) hands port changes to the real-time
  thread through queues, applies them in `begin_tick()`, and frees a closed port's state
  only after the real-time thread has let it go.
- Do not destroy a port while the real-time thread may still call it: swap it out between
  ticks and retire it first. The transport outlives every port it opened (asserted in its
  destructor), and the real-time thread stops ticking it before it is destroyed.
- `send_rendered()` reserves the buffer first; when it cannot (as `send()` would fail),
  `render` is not called. When `render` returns false, or the packet is refused after it
  ran (the XDP TX ring is full), the buffer is released unsent: a batch slot is not
  counted, a UMEM frame goes back to the pool, or, refused by the ring, is freed. A
  transport calls `render` at most once per packet, because the sender pulls its frames
  from the provider inside it.
- `open_rtcp()` and every `RtcpPort` call belong to the control or reactor thread, never
  the real-time one: RTCP is not real-time traffic. A port whose `fd()` is -1 has no
  descriptor that wakes for all its datagrams, and its owner calls `receive()` each
  reactor cycle. The base `Transport` refuses `open_rtcp()` (`operation_not_supported`),
  so test transports need not implement it.
- RTCP DSCP (AES67-2018 §6.2): "Senders should mark outgoing RTCP packets with the same
  DSCP value as the respective RTP stream packets. Receivers should mark outgoing RTCP
  packets with the same DSCP value they would use on RTP packets if transmitting a
  similar stream." `RtcpOptions::dscp` is that value; the caller passes the stream's (or
  would-be stream's) `SendOptions::dscp`, not the RTCP class of its own.

### Sender

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
  them counted lost. `relay_send_offset()`, a packet plus two ticks, is for a provider fed
  from the network (an AES67 receiver's frames). A changed send offset takes effect at the next
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
  `clock-domain:PTPv2 <domain>` from `set_grandmaster()`, `mediaclk:direct=0`, a source
  filter naming this host for a multicast group, and `a=recvonly`, as AES67's examples and
  Dante write it. A `Grandmaster` whose `version` is `ieee802_1as` is written
  `ts-refclk:ptp=IEEE802.1AS-2011:<EUI-64>` with no domain and no `clock-domain`
  (AES67-2018 Clause 8.2, Example 2); the IPMX Info Block's `ts-refclk` follows it.
- Session id and version: a receiver (Dante Controller, say) keys a session on the o=
  session id and origin, so a new id each start leaves stale sessions behind. The id is
  stable across restarts: `set_session_id()`, or by default `stable_session_id()`
  (FNV-1a) of the origin address and the name, so two senders on one host with the same
  name need ids of their own. The version starts at the wall clock's seconds (RFC 8866's
  suggestion, so it grows across restarts too) and goes up by one with each
  `configure()`, grandmaster change and rename. Ids and versions are kept to 63 bits.
  `continue_from()` carries the id and version (and the send offset, grandmaster, media
  clock and name) to a replacement sender, so its next `configure()` announces the same
  session one version on.
- A clock domain other than PTP: the caller ticks the sender with the domain's index
  (`PtpMediaClock::index_at(timebase.to_domain(ptp_now))`, `mediaclock::Timebase`), which
  advances 5 to 7 frames a tick at 48 kHz for a domain within 100s of ppm; `tick()` needs no
  change and sends every packet once, 0, 1 or 2 a tick. `set_sender_clock(true)` makes the
  SDP say `mediaclk:sender` (RFC 7273 §5) instead of `direct=0`, which would claim the RTP
  timestamps are PTP time; `ts-refclk` and `clock-domain` still name the grandmaster, the
  reference the sender reports are in. Such a stream needs its `RtcpSession` with
  `set_timebase()`: the SR is the only statement of its clock.
- Who takes a `mediaclk:sender` stream: RAVENNA and IPMX receivers that follow its sender
  reports (their asynchronous case), and a `Receiver` with `set_sender_clock(true)`. Dante
  in AES67 mode expects `direct=0` on its PTP and refuses or drifts; nothing beyond the SDP
  telling the truth prevents it listing the announcement.

### Receiver

- `configure()` takes the media's group and port, payload type, encoding, channels and
  rate, an RFC 4570 source filter if any, and the RFC 7273 direct media clock offset. It
  prefers the offered format with the receiver's encoding (`Media::prefer()`), else the
  primary one; the encoding must be the receiver's (`encoding_mismatch`), and channels and
  rate too (`format_mismatch`). The ptime need not match: any whole number of frames is
  taken. A stream without a direct media clock is refused (`not_ptp_locked`): its
  timestamps are not PTP time.
- `set_sender_clock(true)` before `configure()` takes only `mediaclk:sender` streams (any
  other fails `not_sender_clocked`, `direct` included, as a direct stream would drift in a
  foreign domain). Such a stream has no offset until `set_offset()`: `drain()` learns its
  SSRC and sequence (so its session takes only that source's reports) but drops its
  packets (counted) until then, and a new source clears the offset. The offset comes from the stream's first IPMX-form SR:
  `sender_clock_offset(report, timebase, fs)` is the RTP timestamp less the domain's index
  at the PTP time named, rounded to the nearest sample. `set_offset()` is an atomic store,
  safe from the reactor while the real-time thread drains; checking later reports against
  it (a stream not in the domain moves) is the caller's.
- Packets of the stream's payload type are accepted. The first one's SSRC is learnt and
  other sources dropped until the learnt one has been silent for a second. The sequence is
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
  header), `payload_type` (not the SDP's), `size` (empty, or not whole frames), `source`
  (not the source filter's, RFC 4570), `ssrc` (another source while the learnt one is
  live), `sequence` (a duplicate, or a jump RFC 3550 Appendix A.1 has not confirmed) and
  `no_offset` (a sender-clocked stream before `set_offset()`). Checked in that order; a
  packet counts once, under the first reason that refuses it.

### RTCP

- Scope: RFC 3550 §6 as a dedicated device needs it. A sender's session sends,
  every interval, one compound packet: an SR, then SDES with its CNAME (§6.1 wants the
  CNAME in every compound packet). A receiver's sends an RR with one report block about
  the stream, then SDES. Both send an empty RR, SDES and BYE on `close()` or destruction; a sender's
  session also sends one, at its next `tick()`, for the SSRC it last saw sending once that is gone
  (stopped by `Sender::pause()`, or replaced by a resumed run's, however quickly), and then none on
  `close()` unless it sent again. The BYE is built on the reactor, never the real-time thread.
  Out of scope: an RFC 3550 wall-clock NTP mode, APP, SDES items past CNAME, RTCP-XR,
  timer reconsideration and member tables (§6.3.3–6.3.7), RTP/RTCP multiplexing (RFC
  5761), and the IPMX schedule (an SR before every Nth media packet, TR-10-1 §8.10.1),
  which a cadence of its own will provide.
- One SR form: the time field is always the PTP truncated time (TR-10-1 §8.7: PTP
  seconds mod 2³², then nanoseconds) and the SR always carries the IPMX Info Block with
  the PCM Media Info Block, which tells any reader how to take the field. AES67-2018 §6.3
  asks only for RFC 3550 §6 and prescribes no derivation of the field, and no AES67
  receiver synchronises from it. The pair is exact: the field is the presentation time
  of the latest sample at or before now, the RTP timestamp that sample's
  (`mediaclk:direct=0`).
- A sender in another clock domain (`set_timebase()`, read per report): the SR pairs the
  domain's index at now with that index's PTP time through the timebase (`from_domain()`,
  within a nanosecond), and none is sent while the timebase is not running. A null
  timebase, or the identity, gives the same bytes as before; the SR path costs one seqlock
  load per report, and the sender's tick is unchanged.
- A receiver's session takes the pair from each of the stream's SRs that carries the IPMX
  Info Block (`RtcpStats::remote_sr`, `RtcpSessionOptions::on_sender_report`); an SR
  without it names no PTP time (it may be RFC 3550's wall clock) and gives no pair.
- The Info Block's strings are the SDP's (`sdp::ts_refclk_value()`,
  `media_clock_value()` of `Sender::media_clock()`); `measuredsamplerate` is the
  nominal rate in PTP time, `round(fs * (1 + r))` in a domain of rate offset r, kept
  until the rate moves a whole hertz from it, a change of it alone one version on (the
  count is the session's; a replacement sender has a new SSRC); the channel order is `Sender::set_channel_order()`, by default undefined
  groups of at most 64 (ST 2110-30's U01–U64), `SMPTE2110.(U02)` or
  `SMPTE2110.(U64,U64,U08)`; the 8-bit channel count is clamped at 255; its version is `Sender::info_block_version()`, one on with every
  `configure()`, grandmaster and channel-order change. TR-10-3 allows sample sizes of
  16 and 24; an L32 stream says 32.
- DSCP (AES67-2018 §6.2): a sender's session marks its reports with its stream's DSCP
  (`Sender::set_dscp()`); a receiver's with `RtcpSessionOptions::dscp`, the DSCP it would
  send a similar stream with. §6.2 recommends AF41 (34) for media and EF (46) for clock;
  `DSCP_MEDIA` is 46, as Dante marks media, and the caller chooses.
- Interval: a sender's minimum is `interval_ms` (1 s by default), between RFC 3550 §6.2's
  5 s and the reduced minimum an active sender may use; a receiver keeps the 5 s fixed
  minimum. `Rfc3550Interval` applies §6.3.1 with two members and one sender over 5 % of
  the stream's bandwidth (the share never binds at AES67 rates), halves the first
  interval, and randomises each. Receivers heard from are not counted into it.
- Round trip: an RR echoes the middle bits of the SR's field, which for the PTP form are
  not RFC 3550's 1/65536 s units, so the sender keeps the PTP time of its last
  `SR_HISTORY` SRs and finds the one an RR's LSR names; the RTT is arrival − that time −
  DLSR, resolved to DLSR's 15.26 µs.
- Jitter (RFC 3550 §6.4.1, Appendix A.8) is computed in `Receiver::accept()` from the
  drain's index as the arrival time, so it is quantised to one tick (125 µs, 6 samples at
  48 kHz), and only the first packet of each drain counts: packets drained together share
  an arrival time, and their timestamp gaps would read as jitter. A diagnostic, not a
  measurement. Loss figures come from the receiver's
  `RtpSequence`; the session forms the interval's fraction from the cumulative
  `expected` and `received` it last reported, so nothing on the real-time thread resets.
- A session hears its own reports when the group loops them back and skips its SSRC.
  A receiver's session takes any SR on the stream's RTCP endpoint before the first media
  packet, and only the stream's SSRC after. A sender's "remote" is whichever receiver
  reported on its stream last.
- The CNAME is the caller's: RFC 3550 §6.5.1's `user@host` shape, as
  `<identity>@<address>`, at most 255 bytes.
- `Receiver::rtcp_options()` takes the endpoint from the SDP: `a=rtcp:` when given,
  else the media's address at port + 1. An `a=rtcp:` unicast address is where reports go
  (RFC 3605), received here at the same port; otherwise, for a unicast stream, reports go
  to the sender (the source filter's source, else the origin address) at the same port. `a=rtcp-mux`
  leaves the stream with no RTCP (`rtcp_multiplexed`).
- Times are PTP nanoseconds supplied by the caller (`on_ready()`, `tick()`); a report
  scheduled far beyond the minimum (PTP stepped back) is re-armed.

### UdpTransport

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
  shared port) is skipped uncounted, and the receiver judges the rest (`Counters::drops`).
- A sender's socket is unconnected and every message names the destination, so a unicast
  receiver's ICMP port-unreachable never fails a send. Multicast goes out of the
  interface with the configured TTL and the sender's DSCP (`SendOptions::dscp`, from
  `Sender::set_dscp()`, by default `rtp::FrameAddressing::DSCP_MEDIA`, 46). A tick's packets are written straight
  into the `SEND_BATCH` slots of the sender's `sendmmsg()` batch.
- An RTCP port is one socket for both directions. For a group it is bound to the group at
  the RTCP port, joined, and sends there with loopback on, so a session hears its own
  reports and must skip its own SSRC; for a unicast peer it is bound to `RtcpOptions::local`
  (by default this address at the destination port), without `SO_REUSEADDR`, and sends to
  the peer: a second port on the same unicast address and port is refused
  (`address_in_use`) rather than splitting the reports between the two.

### XdpTransport

- The XDP program redirects to the socket the untagged IPv4 (IHL 5) UDP frames whose
  destination is exactly one of the joined groups (the rule matches the group's MAC and
  the address's first two octets, which together are the whole address) and passes
  everything else to the kernel. `begin_tick()` drains the RX ring once and hands each
  frame, parsed with `rtp::parse_rtp_frame()`, to the ports for its destination group and
  port; senders write whole frames into UMEM and `end_tick()` kicks the NIC.
- Group membership is still the kernel's: each receive port holds a plain UDP socket
  joined to its group, so IGMP reports go out and the NIC's multicast filter keeps the
  group.
- `begin_tick()` is the only place the port list and the installed rules change: it takes
  new ports from a bounded queue, drops closed ones (handing their state back on a retire
  queue, freed on the control thread's next `open_*()` or port destruction, or by the
  destructor), and reinstalls the rules when the set changed. That is a BPF map update, a
  syscall on the real-time thread, only when receivers come or go.
- TX pool: senders keep `TX_POOL` UMEM frames back, topped up each tick before the fill
  ring is; those are never offered to the kernel. A send takes a frame from the pool,
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
  stealing the host's UDP, a unicast sender ARP); at most `MAX_GROUPS` distinct groups
  (the BPF program's TCAM holds 16 rules); a joined group's frames to ports no receiver
  here listens on do not reach the kernel (the rule cannot see the UDP port), so other
  listeners on the same group miss them; untagged frames only; one queue (`QUEUE`, 0), so the
  NIC must deliver the groups there.
- RSS: a NIC with several RX queues hashes UDP flows over all of them, so `open()` steers
  every RSS bucket to `QUEUE` (`XdpTransportOptions::steer_rss`, on by default;
  `net::set_rss_indirection_all`, `CAP_NET_ADMIN`) and `rss_status()` says whether it could.
  A NIC without RSS reports `operation_not_supported` and needs one combined channel or a
  flow-steering rule instead. The table is left steered at close.
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

- AES67-2018 Annex C.1.1 carries a stream as an IEEE 802.1Q Clause 35 reservation. `srp_tspec(format,
  class)` is its TSpec (Clause 35.2.2.8.4): MaxFrameSize counts the IPv4, UDP and RTP headers, the
  payload and one spare octet, no Ethernet overhead (Annex C.1.1.1); MaxIntervalFrames the packets
  that can start in one class interval (125 us for class A, 250 us for B, Clause 34.4).
- A packet time longer than the interval would reserve a whole frame every interval for one sent
  each packet time, so `srp_format()` cuts the frames per packet to the fewest whose packet time
  reaches the interval: 6 at 48 kHz (exactly 125 us) and at 44.1 kHz (136 us, one frame per
  interval) in class A, 12 at 96 kHz. A packet time already that short is kept. The caller sends
  and announces the cut format.
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
  honours it, `net::launch_time_support`; a build without TX metadata opens without and records
  `launch_time_error()`) gives every frame on the socket a launch time (`LaunchClock`): CLOCK_TAI
  now until `set_launch_lead_ns()` puts a lead in force, then departure plus lead, never earlier than
  the last time committed (`net::LaunchOrder`: igc's queue is a FIFO; a time over 1 ms behind is a
  clock step; a time already past at commit, an overrunning tick, goes at now and is counted in
  `launch_clock().order()`); `clear_launch_lead()` is now again. An untimed send (`send()`, a tick
  without a time) goes at now. RTCP and SAP go by kernel sockets, never through the socket.
- The lead moves the wire time only: the packet leaves at departure plus lead, free of the sending
  CPU's jitter, while its timestamp and the receiver's placement are unchanged, so launch time
  changes no latency; it lets a receiver's link offset cover the lead plus the network transit
  instead of the sender's jitter as well (AES67-2018 Section 7.4). A lead of 0 would be past when
  the tick commits the frame (the NIC sends at once, as without launch time), so the lead is a
  whole tick or more.
- The NIC honours the time only with the socket's queue in launch-time mode, and igc places a
  frame by CLOCK_TAI's offset in its cycle against its own PHC, so the queue may be in that mode
  only while the system clock follows this NIC's PHC: `XdpTransport::settle_launch_time(interface,
  mode)` lays core's mqprio root (`LAUNCH_TIME_QDISC_MAJOR`, every priority mapped away from
  `QUEUE`) with an offloaded etf on the socket's queue (`net::tc::settle_launch_time_queue`) when
  the host finds the clocks agree, and removes them when they no longer do; on igc each switch
  resets the adapter (its PHC time is kept across the reset, `igc_ptp_time_restore`), so the host
  says so. `unsupported` (igb) looks nothing up and sends nothing. None of this has run on an igc
  yet.

## Further reading

- AES67-2018 — interoperability requirements for audio over IP (§6.2 DSCP, §6.3 transport and RTCP).
- RFC 3550 — RTP and RTCP; RFC 3605 — the RTCP attribute; RFC 7273 — clock source signalling; RFC 4570 — source filters.
- VSF TR-10-1:2024 §8.7 and TR-10-3:2024 §11 — the IPMX SR and Info Block.
- [`mediaclock`](MEDIACLOCK_MODULE.md), [`rtp`](RTP_MODULE.md), [`sdp`](SDP_MODULE.md), [`sap`](SAP_MODULE.md).
- statusbar-core `net` module — `XdpContext`, `CompiledRule`, sockets.
