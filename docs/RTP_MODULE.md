[← back to module index](README.md)

# rtp

RFC 3550 RTP for AES67: the fixed header as a wire struct, parsing and writing a packet
in a UDP payload, sequence validation and loss accounting, and RTP inside a raw
Ethernet/IPv4/UDP frame for AF_XDP paths. RTCP (RFC 3550 §6): SR, RR, SDES and BYE,
compound packets, the IPMX Info Block (VSF TR-10-1 §8.7, TR-10-3 §11), PTP truncated
time and the report interval.

## Overview

Everything works on spans of caller-owned memory and never allocates. The UDP-payload
functions (`parse_rtp`, `serialize_rtp`, `serialize_rtp_header`) serve a socket, which
hands over the payload; the frame functions (`parse_rtp_frame`, `serialize_rtp_frame`,
`serialize_rtp_frame_headers`) serve a raw path that sees whole Ethernet frames, peeling
or writing the Ethernet, IPv4 and UDP headers around the same RTP code. The `*_header(s)`
writers leave room for a payload the caller renders in place afterwards, so a sender
copies its frames once, straight into the transmit buffer.

The RTCP half is pure as well: `parse_rtcp` walks a compound packet and returns views
into it (report blocks, the SR's profile-specific extension, CNAME, BYE sources);
`RtcpWriter` appends packets to a caller's buffer. `rtcp_ipmx` encodes and decodes the
IPMX Info Block an IPMX SR carries as that extension. `rtcp_time` and `rtcp_interval`
are constexpr arithmetic with no clock or generator of their own: the caller passes PTP
nanoseconds and injects the random generator.

## Key types

- `RtpHeader` — the 12-byte RFC 3550 Figure 1 fixed header, packed, network order, with bit accessors and `init()` for the header AES67 senders emit.
- `RtpPacket` — a parsed header and a view of its payload (CSRCs, extension and padding removed).
- `RtpSequence` — one source's sequence state (RFC 3550 Appendix A.1) and loss figures (Appendix A.3, §6.4.1 `report()`).
- `FrameAddressing` — the MACs, optional VLAN tag, IPv4 addresses, UDP ports, TTL and DSCP around a packet in a frame.
- `RtpFrame` — a parsed frame: its addressing, its UDP payload and the RTP packet.
- `multicast_mac()` — an IPv4 group's Ethernet address (RFC 1112 §6.4); `rtp_frame_overhead()` — 42 or 46 bytes.
- `RtcpHeader`, `SenderInfo`, `ReportBlock` — the RFC 3550 §6.4.1 wire structs.
- `RtcpCompound` — what `parse_rtcp()` found: the first SR, RR, SDES chunk and BYE, as views (`RtcpSenderReport`, `RtcpReceiverReport`, `RtcpSdes`, `RtcpBye`).
- `RtcpWriter` — sticky-error, chained writer of SR, RR, SDES (CNAME, NAME) and BYE.
- `IpmxInfoBlock`, `IpmxPcmMediaInfo` — the IPMX Info Block and its PCM Media Info Block.
- `PtpTruncated` — an SR time field as IPMX fills it: PTP seconds mod 2³² and nanoseconds.
- `RtcpIntervalParams` — the inputs of the §6.3.1 interval.

## Quick example

```cpp
#include "statusbar/rtp/rtp_header.hpp"
#include "statusbar/rtp/rtp_sequence.hpp"

#include <array>

using namespace statusbar;

auto echo(std::span<uint8_t const> datagram, rtp::RtpSequence& sequence) -> bool
{
    auto const packet = rtp::parse_rtp(datagram);
    if (!packet || !sequence.update(packet->header.sequence.get())) {
        return false;  // malformed, on probation, or an unconfirmed jump
    }
    std::array<uint8_t, 1500> out{};
    rtp::RtpHeader header{};
    header.init(packet->header.payload_type(), packet->header.sequence.get(), packet->header.timestamp.get(), 0x1234);
    return rtp::serialize_rtp(header, packet->payload, out).has_value();
}
```

## Headers

- `statusbar/rtp/rtp_header.hpp` — `RtpHeader`, `RtpPacket`, `parse_rtp()`, `serialize_rtp()`, `serialize_rtp_header()`.
- `statusbar/rtp/rtp_sequence.hpp` — `RtpSequence` (header-only).
- `statusbar/rtp/rtp_frame.hpp` — `FrameAddressing`, `RtpFrame`, `parse_rtp_frame()`, `serialize_rtp_frame()`, `serialize_rtp_frame_headers()`, `multicast_mac()`.
- `statusbar/rtp/rtcp_packet.hpp` — the RTCP wire structs, `parse_rtcp()`, `RtcpWriter`, `padded_to_word()`.
- `statusbar/rtp/rtp_error.hpp` — `RtpError` (`statusbar.rtp`), what `RtcpWriter::status()` reports.
- `statusbar/rtp/rtcp_ipmx.hpp` — `IpmxInfoBlock`, `serialize_ipmx_info_block()`, `parse_ipmx_info_block()`.
- `statusbar/rtp/rtcp_time.hpp` — `ptp_truncated_from_ns()`, `ns_from_ptp_truncated()`, `lsr_of()`, `dlsr_from_ns()`, `rtt_ns()` (header-only, constexpr).
- `statusbar/rtp/rtcp_interval.hpp` — `deterministic_rtcp_interval()`, `rtcp_interval()` (header-only, constexpr).

## Dependencies

- **Statusbar modules:** `buffer` (wire-struct traits), `status` (`RtcpWriter::status()`), `ieee` (`octet_t`/`doublet_t`/`quadlet_t`, `Eui48`, `VlanTag`), `ip` (`IPv4Address`, IPv4/UDP headers, checksum).
- **System / external:** `<span>`, `<optional>`, `<compare>`.

## Notes & caveats

- `parse_rtp()` does not judge the payload type. AES67 never multiplexes RTCP on the RTP
  port, so a marked packet of any payload type is RTP; the caller matches the type to the
  stream's SDP.
- `serialize_rtp()` writes a fixed-header-only packet and clears the padding, extension
  and CSRC-count bits, since none of those sections is written.
- `parse_rtp_frame()` refuses anything that is not unfragmented IPv4/UDP with a correct
  header checksum and lengths that agree with each other and with the frame. Trailing
  Ethernet padding is allowed. It does not judge addresses, ports or the payload type.
- `serialize_rtp_frame()` writes what an AES67 sender emits: IHL 5, DF set, the IPv4
  header checksum computed, and a zero (absent) UDP checksum, which IPv4 permits and
  AES67 receivers accept. Short frames are not padded; the NIC pads them.
- `RtpSequence` keeps a new source on probation until `MIN_SEQUENTIAL` packets arrive in
  order; gaps below `MAX_DROPOUT` are losses; a larger jump is believed only when the next
  packet follows it (the sender restarted); late and duplicate packets are accepted but
  do not move the high-water mark. `lost()` goes negative when duplicates outnumber
  losses, as RFC 3550 counts them.
- `parse_rtcp()` applies RFC 3550 Appendix A.2: version 2 throughout, an SR or RR first
  and unpadded, padding only on the last sub-packet, lengths summing to the datagram. A bare SR (what
  IPMX sends) passes; APP, XR and other types are skipped, as TR-10-1 §8.7 asks IPMX
  receivers to tolerate. Only the first chunk of an SDES packet is read.
- The SR's 64-bit time field is whatever the sender wrote. The library writes only the
  PTP truncated form (TR-10-1 §8.7 Figure 2: PTP seconds, then nanoseconds), and a reader
  takes it as such only when the SR carries an IPMX Info Block (tag 0x5831), never by
  looking at the values. There is no NTP form and no UTC offset.
- An RR's LSR echoes the middle 32 bits of the SR's field; for a PTP-form SR its low
  half is nanoseconds ÷ 65536, not 1/65536 s. A sender therefore keeps the PTP time of
  each SR it sent, finds it by the echoed LSR, and computes the round trip with
  `rtt_ns()` (arrival − sent − DLSR); DLSR's 15.26 µs unit is the resolution.
- `rtcp_interval()` is §6.3.1 without reconsideration or member tables (§6.3.3–6.3.7):
  the caller states the members and senders it assumes. The first report uses half the
  minimum (§6.2); every interval is randomised in [0.5, 1.5) and divided by e − 3/2.
- `serialize_ipmx_info_block()` NUL-pads `ts-refclk` to 64 bytes and `mediaclk` to 12; a
  string that fills its field has no NUL. The channel-order string is padded to 32 bits.

## Further reading

- RFC 3550 §5.1, §6 (§6.1 compound packets, §6.3.1 interval, §6.4 SR/RR, §6.5 SDES, §6.6 BYE), Appendix A.1–A.3, A.7.
- VSF TR-10-1:2024 §8.7 (IPMX SR, Info Block, PTP truncated time); VSF TR-10-3:2024 §11–12 (PCM Media Info Block, the example SR the tests use).
- RFC 1112 §6.4 — IPv4 multicast to Ethernet address mapping.
- [`aes67`](AES67_MODULE.md) — the transports built on these functions.
