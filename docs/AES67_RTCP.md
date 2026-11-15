[← back to docs index](README.md)

# AES67 RTCP

What the `aes67` module's `RtcpSession` sends and reads beside a stream: its scope, the sender
schedules, the IPMX sender report, intervals, jitter and the CNAME; the module overview is
[AES67_MODULE.md](AES67_MODULE.md).

## RTCP

- Scope: RFC 3550 Section 6 as a dedicated device needs it. A sender's session sends, on VSF TR-10-1
  Section 8.10.1's schedule, one compound packet per report: an SR, then SDES with its CNAME (Section 6.1 wants the
  CNAME in every compound packet). A receiver's sends an RR with one report block about
  the stream, then SDES. Both send an empty RR, SDES and BYE on `close()` or destruction; a sender's
  session also sends one, at its next `tick()`, for the SSRC it last saw sending once that is gone
  (stopped by `Sender::pause()`, or replaced by a resumed run's, however quickly; `SendState::sent` says whether
  a run has sent, so a clock step that moves `next_index` back onto `first_index` is not a stop), and then none on
  `close()` unless it sent again. The BYE is built on the reactor, never the real-time thread.
  Out of scope: an RFC 3550 wall-clock NTP mode, APP, SDES items past CNAME, RTCP-XR,
  timer reconsideration and member tables (Section 6.3.3–6.3.7), and RTP/RTCP multiplexing (RFC
  5761).
- A sender's schedule is `RtcpSessionOptions::reports`: `rfc3550` (the default) sends an SR at RFC 3550 Section 6.3.1's
  randomised interval (Tmin `SENDER_INTERVAL_MS`, 1 s), naming the next packet to send, with no first-packet hold and
  no IPMX declaration; `ipmx` follows TR-10-1 below. Both carry the Info Block, which says how the time field reads.
- The `ipmx` schedule (VSF TR-10-1 Section 8.10.1): one SR for a run's first packet, then one every N = int(10 ms / ptime)
  packets (`REPORT_SPACING_US`; 10 at 1 ms, 80 at 125 µs), each naming its packet's RTP timestamp and sent before
  that packet and after the one N before. The sender places a run's first packet `FIRST_REPORT_LEAD_US` (20 ms)
  ahead, publishes it (`SendState::first_index`), and sends it when due, whether or not its SR went: audio never
  waits on RTCP. The session sends that SR at its next tick. A clock step back past the packet, or forward past
  a burst, places it afresh, so no SR names a packet never sent. Later SRs go once the previous grid packet has left
  (`SendState::next_index`); `next_wake_ns()` says when, midway through the next SR's window, for the caller's
  timer. One whose packet left first (the caller late) is passed over; a clock step back restarts the grid. A run
  paused before its first packet sent none and gets no BYE. About 100 SRs a second of about
  180 octets: 0.15 Mbit/s against a 9 Mbit/s 8-channel L24 stream.
- One SR form: the time field is the sender's clock in the PTP truncated format (TR-10-1 Section 8.7: seconds mod 2³²,
  then nanoseconds) and the SR always carries the IPMX Info Block with the PCM Media Info Block. The field is PTP
  time when `ts-refclk` names a PTP grandmaster; under `localmac` it is the sender's free-running clock (Section 7.1,
  Section 10.4). AES67-2023 Clause 6.3 asks only for RFC 3550 Section 6 and prescribes no derivation of the field, and no AES67
  receiver synchronises from it. The pair is exact: the field is the time the SR's packet's first sample is
  sampled (`time_for(index)`), the RTP timestamp that sample's (`mediaclk:direct=0`).
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
- A receiver's interval keeps RFC 3550 Section 6.2's 5 s fixed minimum; an `rfc3550` sender's a chosen 1 s. `Rfc3550Interval` applies Section 6.3.1 with two
  members and one sender over 5 % of the stream's bandwidth (the share never binds at AES67 rates), halves the
  first interval, and randomises each. Receivers reporting are not counted into it.
- Round trip: an RR echoes the middle bits of the SR's field, which for the PTP form are
  not RFC 3550's 1/65536 s units, so the sender keeps the PTP time of its last
  `SR_HISTORY` SRs (128, 1.28 s of them) and finds the one an RR's LSR names; the RTT is arrival − that time −
  DLSR, resolved to DLSR's 15.26 µs.
- Jitter (RFC 3550 Section 6.4.1, Appendix A.8) is computed in `Receiver::accept()` from the
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
- The CNAME is the caller's: RFC 3550 Section 6.5.1's `user@host` shape, as
  `<identity>@<address>`, at most 255 bytes.
- `Receiver::rtcp_options()` reads the endpoint from the SDP: `a=rtcp:` when given,
  else the media's address at port + 1. An `a=rtcp:` unicast address is where reports go
  (RFC 3605), received here at the same port; otherwise, for a unicast stream, reports go
  to the sender (the source filter's source, else the origin address) at the same port. `a=rtcp-mux`
  leaves the stream with no RTCP (`rtcp_multiplexed`).
- Times are PTP nanoseconds supplied by the caller (`on_ready()`, `tick()`); a report
  scheduled far beyond the minimum (PTP stepped back) is re-armed.
