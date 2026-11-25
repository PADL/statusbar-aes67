[← back to docs index](README.md)

# AES67 senders

How the `aes67` module's `Sender` frames, times and describes a stream; the module overview is
[AES67_MODULE.md](AES67_MODULE.md).

## Sender

- When a packet is due, which run it belongs to and its sequence number and timestamp are a
  `PacketSchedule`'s (the grid, the due rule, the resynchronisation below, a held start, each
  run's SSRC and sequence); the `Sender` keeps the session and SDP, the grandmaster, report
  holds, counters and rendering.
- Packet times (AES67-2023 Clause 7.2): a sender sends any whole number of frames per
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
  `Transport::local_mac()`, the sender's own clock, VSF TR-10-1 Section 10.4; `references()`), `mediaclk:direct=0`, a source
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
  `configure()`, grandmaster change (`traceable` included), rename and change of attributes. Ids and versions are kept
  to 63 bits. `continue_from()` carries the id and version (and the send offset, grandmaster, media
  clock, name and attributes) to a replacement sender, so its next `configure()` announces the same
  session one version on.
- `set_attributes()` gives `session()` session-level attributes the caller interprets (`sdp::Attribute`, such as
  an `x-` extension naming the stream in a control protocol), written after the sender's own, in order.
- `set_st2110(true)` gives `session()` ST 2110-30's form (Section 6.2): the channel order as an fmtp
  (`a=fmtp:<pt> channel-order=`, `channel_order()`: the one set, or undefined groups) and `a=maxptime`, the packet
  time. Off by default, so an AES67 receiver sees AES67's attributes alone. A change, and a channel order changed
  while it is on, bumps the session version.
- While an ipmx `RtcpSession` holds the sender (a `Sender::ReportHold` from `hold_reports()`, which the session owns
  while open; the holds are counted, so one session closing lifts no other's), `session()` declares IPMX in
  the fmtp (`a=fmtp:<pt> IPMX`, after any channel order; VSF TR-10-1 Section 10.1): its SRs then follow Section 8.10.1. The
  declaration appearing or going bumps the session version. Without a session the SDP is AES67's alone.
- A clock domain other than PTP: the caller ticks the sender with the domain's index
  (`PtpMediaClock::index_at(timebase.to_domain(ptp_now))`, `mediaclock::Timebase`), which
  advances 5 to 7 frames a tick at 48 kHz for a domain within 100s of ppm; `tick()` needs no
  change and sends every packet once, 0, 1 or 2 a tick. `set_sender_clock(true)` makes the
  SDP say `mediaclk:sender` (RFC 7273 Section 5) instead of `direct=0`, which would claim the RTP
  timestamps are PTP time; `ts-refclk` and `clock-domain` still name the grandmaster, the
  reference the sender reports are in. Such a stream needs its `RtcpSession` with
  `set_timebase()`: the SR is the only statement of its clock.
- Who accepts a `mediaclk:sender` stream: RAVENNA and IPMX receivers that follow its sender
  reports (their asynchronous case), and a `Receiver` with `set_sender_clock(true)`. Dante
  in AES67 mode expects `direct=0` on its PTP and refuses or drifts; nothing beyond the SDP
  telling the truth prevents it listing the announcement.
