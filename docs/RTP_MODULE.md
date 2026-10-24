[← back to module index](README.md)

# rtp

RFC 3550 RTP for AES67: the fixed header as a wire struct, parsing and writing a packet
in a UDP payload, sequence validation and loss accounting, and RTP inside a raw
Ethernet/IPv4/UDP frame for AF_XDP paths.

## Overview

Everything works on spans of caller-owned memory and never allocates. The UDP-payload
functions (`parse_rtp`, `serialize_rtp`, `serialize_rtp_header`) serve a socket, which
hands over the payload; the frame functions (`parse_rtp_frame`, `serialize_rtp_frame`,
`serialize_rtp_frame_headers`) serve a raw path that sees whole Ethernet frames, peeling
or writing the Ethernet, IPv4 and UDP headers around the same RTP code. The `*_header(s)`
writers leave room for a payload the caller renders in place afterwards, so a sender
copies its frames once, straight into the transmit buffer.

## Key types

- `RtpHeader` — the 12-byte RFC 3550 Figure 1 fixed header, packed, network order, with bit accessors and `init()` for the header AES67 senders emit.
- `RtpPacket` — a parsed header and a view of its payload (CSRCs, extension and padding removed).
- `RtpSequence` — one source's sequence state (RFC 3550 Appendix A.1) and loss figures (Appendix A.3, §6.4.1 `report()`).
- `FrameAddressing` — the MACs, optional VLAN tag, IPv4 addresses, UDP ports, TTL and DSCP around a packet in a frame.
- `RtpFrame` — a parsed frame: its addressing, its UDP payload and the RTP packet.
- `multicast_mac()` — an IPv4 group's Ethernet address (RFC 1112 §6.4); `rtp_frame_overhead()` — 42 or 46 bytes.

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

## Dependencies

- **Statusbar modules:** `buffer` (wire-struct traits), `ieee` (`octet_t`/`doublet_t`/`quadlet_t`, `Eui48`, `VlanTag`), `ip` (`IPv4Address`, IPv4/UDP headers, checksum).
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

## Further reading

- RFC 3550 §5.1, Appendix A.1, Appendix A.3, §6.4.1.
- RFC 1112 §6.4 — IPv4 multicast to Ethernet address mapping.
- [`aes67`](AES67_MODULE.md) — the transports built on these functions.
