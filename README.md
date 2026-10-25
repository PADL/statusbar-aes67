# statusbar-aes67

AES67 for the [statusbar](https://codeberg.org/statusbar) stack: the media clock, RTP,
SDP, SAP and stream I/O an AES67/AVB bridge is built on. A package of the statusbar umbrella; it depends
on `statusbar-core`.

| Module | What it is | Docs |
| --- | --- | --- |
| `mediaclock` | `PtpMediaClock`: sample index ↔ PTP time, the AES67 `mediaclk:direct=0` clock, and RTP timestamp unwrapping; `Timebase`/`TimebaseFit`, a non-PTP media clock fitted against PTP | [MEDIACLOCK_MODULE.md](docs/MEDIACLOCK_MODULE.md) |
| `rtp` | RFC 3550: `RtpHeader`, `parse_rtp` / `serialize_rtp` (`serialize_rtp_header`) over a UDP payload, `RtpSequence` (Appendix A.1 validation and loss counts), `parse_rtp_frame` / `serialize_rtp_frame` (`serialize_rtp_frame_headers`) for RTP in a raw Ethernet/IPv4/UDP frame; RTCP: `parse_rtcp` / `RtcpWriter` (SR, RR, SDES, BYE, compound packets), the IPMX Info Block (VSF TR-10-1 §8.7, TR-10-3 §11), PTP truncated time, the §6.3.1 interval | [RTP_MODULE.md](docs/RTP_MODULE.md) |
| `sdp` | RFC 8866 session descriptions for AES67: origin, connection (TTL), L16/L24/L32/AM824 audio media with alternative formats, ptime, RFC 7273 `ts-refclk` / `mediaclk:direct`, RAVENNA `clock-domain`, RFC 4570 source filters, RFC 3605 `rtcp` | [SDP_MODULE.md](docs/SDP_MODULE.md) |
| `sap` | RFC 2974 SAP: packets; `SapAnnouncer`, announcing sessions to 239.255.255.255:9875 every 30 s with deletion on withdraw; `SapDirectory`, the table of announced sessions, and `SapReceiver`, which joins the SAP groups and feeds it | [SAP_MODULE.md](docs/SAP_MODULE.md) |
| `aes67` | AES67 streams: `Receiver` (SDP-configured, SSRC/sequence validation, RTP timestamp → PTP sample index, due-tick gate) and `Sender` (packets on the sample-index grid with a staggerable phase, `mediaclk:direct=0`, its own SDP, payloads rendered in place), over `UdpTransport` (recvmmsg/sendmmsg) or `XdpTransport` (AF_XDP); `RtcpSession`, a stream's RTCP (IPMX-form SR with SDES CNAME, RR, BYE, statistics and round trip) on the transport's `RtcpPort` | [AES67_MODULE.md](docs/AES67_MODULE.md) |

The module index is [docs/README.md](docs/README.md).

Status: early development.
