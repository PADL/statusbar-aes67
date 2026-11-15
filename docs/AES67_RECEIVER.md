[← back to docs index](README.md)

# AES67 receivers

How the `aes67` module's `Receiver` accepts a stream from its SDP, places its packets and counts
what it refuses; the module overview is [AES67_MODULE.md](AES67_MODULE.md).

## Receiver

- `configure()` accepts the media's group and port, payload type, encoding, channels and
  rate, an RFC 4570 source filter if any, and the RFC 7273 direct media clock offset. It
  prefers the offered format with the receiver's encoding (`Media::prefer()`), else the
  primary one; the encoding must be the receiver's (`encoding_mismatch`), and channels and
  rate too (`format_mismatch`). The receiver's ptime need not match the SDP's: each
  packet's frames are counted from its payload (AES67-2023 Clause 7.2), up to the SDP's
  ptime, or its maxptime when longer; a longer packet fails `size`. Clause 7.2 says a receiver
  should not rely on the SDP's packet time; a dedicated device bounds what one packet hands its sink by it all the same. A stream without a direct media clock is
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
  there is no gate.
- Counters: `packets` and `frames` handed on; `lost`, frames in sequence gaps not (yet)
  filled; `late`, frames that arrived out of order (after a later packet), still handed
  on; `dropped`, packets refused, and `drops` the same by `DropReason`: `not_rtp` (no RTP
  header), `payload_type` (not the SDP's), `size` (empty, not whole frames, or more than
  the ptime or maxptime holds), `source` (not the source filter's, RFC 4570, or the learnt SSRC from
  another address), `ssrc` (another source while the learnt one is
  live), `sequence` (a duplicate, or a jump RFC 3550 Appendix A.1 has not confirmed) and
  `no_offset` (a sender-clocked stream before `set_offset()`). Checked in that order; a
  packet counts once, under the first reason that refuses it.
