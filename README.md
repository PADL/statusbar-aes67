# statusbar-aes67

AES67 for the [statusbar](https://codeberg.org/statusbar) stack: the media clock, RTP
and SDP an AES67/AVB bridge is built on. A package of the statusbar umbrella; it depends
on `statusbar-core`.

| Module | What it is |
| --- | --- |
| `mediaclock` | `PtpMediaClock`: sample index ↔ PTP time, the AES67 `mediaclk:direct=0` clock, and RTP timestamp unwrapping |
| `rtp` | RFC 3550: `RtpHeader` (wire struct), `parse_rtp` / `serialize_rtp` (or `serialize_rtp_header`, for a payload written in place) over a UDP payload, `RtpSequence` (Appendix A.1 validation and loss counts), `parse_rtp_frame` / `serialize_rtp_frame` (`serialize_rtp_frame_headers`) for RTP in a raw Ethernet/IPv4/UDP frame |
| `sdp` | RFC 8866 session descriptions for AES67: parse and write origin, connection (TTL), L16/L24 audio media, ptime, RFC 7273 `ts-refclk` / `mediaclk:direct`, RAVENNA `clock-domain`, RFC 4570 source filters |
| `sap` | RFC 2974 SAP: packet build/parse, and `SapAnnouncer`, a `net::Pollable` announcing sessions to 239.255.255.255:9875 every 30 s with deletion on withdraw; `SapDirectory`, the table of announced sessions (announce, newer version, deletion, timeout), and `SapReceiver`, a `net::Pollable` that joins the SAP groups and feeds it, for receivers that subscribe to a session by name |
| `aes67` | AES67 streams: `Receiver` (SDP-configured, SSRC/sequence validation, RTP timestamp → PTP sample index) and `Sender` (packets on the sample-index grid, `mediaclk:direct=0`, its own SDP, each payload rendered by the provider straight into the transport's TX buffer through `SendPort::send_rendered`), over `UdpTransport` (recvmmsg/sendmmsg) or `XdpTransport` (AF_XDP with `rtp_frame`) |

Status: early development. Design: InfernoAES70 `BRIDGE.md`.
