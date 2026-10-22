# statusbar-aes67

AES67 for the [statusbar](https://codeberg.org/statusbar) stack, and the media bus
an AES67/AVB bridge is built on. A package of the statusbar umbrella; it depends on
`statusbar-core`.

| Module | What it is |
| --- | --- |
| `mediabus` | `PtpMediaClock` (sample index ↔ PTP time, the AES67 `mediaclk:direct=0` clock) and `FrameRing` (one stream's audio in its own wire format, addressed by sample index; memcpy in, memcpy or zero-copy out) |

Status: early development. Design: InfernoAES70 `BRIDGE.md`.
