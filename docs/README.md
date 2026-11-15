# statusbar-aes67 documentation

The `padl-aes67` package is the media clock, RTP, SDP, SAP and AES67 stream I/O an
AES67/AVB bridge is built on. Each module is documented in its own overview file; pick a
module for an orientation, then read its headers under `padl/<module>/`.

## Module overviews

| Module     | Purpose                                                                           | Link                                         |
|------------|-----------------------------------------------------------------------------------|----------------------------------------------|
| mediaclock | `PtpMediaClock`: sample index ↔ PTP time, RTP and AVTP timestamps (RFC 7273); `Timebase`, `TimebaseEstimator`: a clock domain fitted against PTP. | [MEDIACLOCK_MODULE.md](MEDIACLOCK_MODULE.md) |
| rtp        | RFC 3550 header, parse and write, sequence validation, RTP in raw Ethernet frames; RTCP and the IPMX Info Block. | [RTP_MODULE.md](RTP_MODULE.md)               |
| sdp        | RFC 8866 session descriptions for AES67: parse and write.                         | [SDP_MODULE.md](SDP_MODULE.md)               |
| sap        | RFC 2974 SAP: announcer, session directory and receiver.                          | [SAP_MODULE.md](SAP_MODULE.md)               |
| aes67      | AES67 receivers and senders over UDP or AF_XDP transports; RTCP sessions.         | [AES67_MODULE.md](AES67_MODULE.md)           |

## Topic guides

In-depth references that go beyond the per-module overviews:

- [AES67_SENDER.md](AES67_SENDER.md) — a `Sender`'s packet times, index grid, send offset and phase stagger, silence, SDP and session id, and clock domains.
- [AES67_RECEIVER.md](AES67_RECEIVER.md) — what a `Receiver` accepts from SDP, where it receives, sender-clocked streams, the learnt SSRC and source, the due-tick gate and counters.
- [AES67_RTCP.md](AES67_RTCP.md) — `RtcpSession`: scope, the `rfc3550` and `ipmx` schedules, the IPMX sender report, intervals, jitter and CNAME.
- [AES67_TRANSPORTS.md](AES67_TRANSPORTS.md) — the transport contract, `UdpTransport`, and streams under SRP.
- [AES67_XDP.md](AES67_XDP.md) — `XdpTransport`: rules, group membership, queues and RSS, and launch time.
