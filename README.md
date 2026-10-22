# statusbar-aes67

AES67 for the [statusbar](https://codeberg.org/statusbar) stack, and the media bus
an AES67/AVB bridge is built on. A package of the statusbar umbrella; it depends on
`statusbar-core`.

| Module | What it is |
| --- | --- |
| `mediaclock` | `PtpMediaClock`: sample index ↔ PTP time, the AES67 `mediaclk:direct=0` clock, and RTP timestamp unwrapping |
| `rtp` | RFC 3550: `RtpHeader` (wire struct), `parse_rtp` / `serialize_rtp` over a UDP payload, `RtpSequence` (Appendix A.1 validation and loss counts) |

Status: early development. Design: InfernoAES70 `BRIDGE.md`.
