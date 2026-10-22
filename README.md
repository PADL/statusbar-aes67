# statusbar-aes67

AES67 for the [statusbar](https://codeberg.org/statusbar) stack, and the media bus
an AES67/AVB bridge is built on. A package of the statusbar umbrella; it depends on
`statusbar-core`.

| Module | What it is |
| --- | --- |
| `mediaclock` | `PtpMediaClock`: sample index ↔ PTP time, the AES67 `mediaclk:direct=0` clock, and RTP timestamp unwrapping |
| `rtp` | RFC 3550: `RtpHeader` (wire struct), `parse_rtp` / `serialize_rtp` over a UDP payload, `RtpSequence` (Appendix A.1 validation and loss counts) |
| `sdp` | RFC 8866 session descriptions for AES67: parse and write origin, connection (TTL), L16/L24 audio media, ptime, RFC 7273 `ts-refclk` / `mediaclk:direct`, RFC 4570 source filters |

Status: early development. Design: InfernoAES70 `BRIDGE.md`.
