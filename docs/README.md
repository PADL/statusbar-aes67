# statusbar-aes67 documentation

The `statusbar-aes67` package is the media clock, RTP, SDP, SAP and AES67 stream I/O an
AES67/AVB bridge is built on. Each module is documented in its own overview file; pick a
module for an orientation, then read its headers under `statusbar/<module>/`.

## Module overviews

| Module     | Purpose                                                                           | Link                                         |
|------------|-----------------------------------------------------------------------------------|----------------------------------------------|
| mediaclock | `PtpMediaClock`: sample index ↔ PTP time, RTP and AVTP timestamps (RFC 7273).     | [MEDIACLOCK_MODULE.md](MEDIACLOCK_MODULE.md) |
| rtp        | RFC 3550 header, parse and write, sequence validation, RTP in raw Ethernet frames; RTCP and the IPMX Info Block. | [RTP_MODULE.md](RTP_MODULE.md)               |
| sdp        | RFC 8866 session descriptions for AES67: parse and write.                         | [SDP_MODULE.md](SDP_MODULE.md)               |
| sap        | RFC 2974 SAP: announcer, session directory and receiver.                          | [SAP_MODULE.md](SAP_MODULE.md)               |
| aes67      | AES67 receivers and senders over UDP or AF_XDP transports; RTCP sessions.         | [AES67_MODULE.md](AES67_MODULE.md)           |
