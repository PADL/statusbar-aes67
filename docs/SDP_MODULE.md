[← back to module index](README.md)

# sdp

RFC 8866 session descriptions for AES67 audio streams: `parse()` into a `Session`,
`to_string()` back to text, covering the subset AES67, RAVENNA and Dante devices write.

## Overview

The module reads and writes an origin, a session name, IPv4 connection data (multicast
TTL included), and audio media over RTP/AVP with L16, L24, L32 and AM824 rtpmaps, ptime,
the RFC 7273 reference clock and media clock (`ts-refclk:ptp=...`,
`mediaclk:direct=<offset>`), RAVENNA's `clock-domain:PTPv2 <n>`, RFC 4570 source
filters, the RFC 3605 RTCP port (`a=rtcp:`), `a=maxptime` (RFC 8866 Section 6.5) and the ST 2110-30
Section 6.2.2 channel order (`a=fmtp:<pt> channel-order=SMPTE2110.(...)`) with VSF TR-10-1's `IPMX` declaration beside it. A session may hold several audio media (ST 2022-7 sends the same stream twice);
other media are skipped, and attributes nothing here uses are ignored. Connection, clock,
media clock, source filter, clock domain and direction may be given at session or media
level; the `Session::effective_*()` accessors resolve them (media level wins).

## Key types

- `Session` — origin (`username`, `session_id`, `session_version`, `origin_address`), name, session-level attributes and `media`.
- `Media` — one `m=audio` section: port, the primary format, up to `MAX_ALTERNATIVES` further offered formats, ptime and media-level attributes; `prefer(encoding)` promotes an offered format.
- `PayloadFormat` — one rtpmap: payload type, `Encoding`, rate, channels.
- `Encoding` — `l16`, `l24`, `l32`, `am824`; `bytes_per_sample()`, `encoding_name()`.
- `Connection`, `PtpReference`, `MediaClock` (`MediaClockKind`), `SourceFilter`, `RtcpAttribute`, `Direction` — the attributes above.
- `PtpVersion` — `ieee1588_2002`, `ieee1588_2008`, `ieee1588_2019`, `ieee802_1as`, `other`; `ptp_version_name()`.
- `ReferenceClocks` — one level's `ts-refclk` lines: the PTP references and the other sources (up to `MAX` each); `ReferenceClocks::of()`, `PtpReference::naming()` build a sender's.
- `Session::effective_clocks()`, `Session::names_clock()`, `PtpReference::same_clock()` — the references in force for a media, and whether one of them names a given clock (AES67-2023 Clause 8.2).
- `ts_refclk_value()`, `media_clock_value()` — the text after `a=ts-refclk:` and `a=mediaclk:`, as written; the IPMX Info Block carries the same strings. `localmac_value()` — `localmac=` with a MAC (RFC 7273 Section 4.8; VSF TR-10-1 Section 10.4), for `ReferenceClocks::other`.
- `Media::ipmx` — the primary format's fmtp declares `IPMX` (VSF TR-10-1 Section 10.1).
- `channel_order_channels()` — the channels an ST 2110-30 channel order names, or nullopt for another form.
- `SdpError` — `enum class`, category `statusbar.sdp`.

## Quick example

```cpp
#include "statusbar/sdp/sdp.hpp"

using namespace statusbar;

auto ptp_offset(std::string_view text) -> std::optional<uint32_t>
{
    auto session = sdp::parse(text);
    if (!session || session->media.empty()) {
        return std::nullopt;
    }
    auto media = session->media.front();
    (void)media.prefer(sdp::Encoding::l24);           // accept an L24 offer if there is one
    return session->effective_media_clock_offset(media);  // nullopt: not PTP-locked
}
```

## Headers

- `statusbar/sdp/sdp.hpp` — the session types, `parse()`, `parse_origin_line()`, `to_string()`, `ts_refclk_value()`, `media_clock_value()`.
- `statusbar/sdp/sdp_error.hpp` — `SdpError`, `sdp_error_category()`, `make_error_code()`.

## Dependencies

- **Statusbar modules:** `ieee` (`Eui64`), `ip` (`IPv4Address`, parsing and formatting), `status`.
- **System / external:** `<string>`, `<vector>`, `<optional>`, `<format>`.

## Notes & caveats

- `Media::channel_order` is the primary format's `channel-order=` among its fmtp's `;`-separated parameters (the
  name in any case), kept only in the form `channel_order_channels()` counts and written only so, after the
  rtpmaps; another fmtp parameter, a malformed order and a malformed `maxptime` are ignored. Whether the
  order names the media's channels is the consumer's to check. A payload type's first well-formed order counts,
  over all its fmtp lines.
- `Media::ipmx` is a bare `IPMX` among those parameters (VSF TR-10-1 Section 10.1), any case; it is written last,
  `a=fmtp:<pt> channel-order=...; IPMX` or `a=fmtp:<pt> IPMX`. Other IPMX parameters (`measuredsamplerate`, a
  baseband sender's, Section 10.3) are read past and never written.

- Several rtpmaps on one m= line: every offered payload type with a supported rtpmap is
  kept. The primary format is the first on the m= line (RFC 8866 §5.14: the order of
  preference), not the first rtpmap line; the rest go to `alternatives` in m= line order,
  up to three, and further ones are dropped. The writer lists the alternatives on the m=
  line and as rtpmaps, so a parsed session writes back as it was offered. A consumer
  that needs one encoding calls `Media::prefer()` and then checks the primary format.
- A media offering only other encodings, or only static payload types without an rtpmap
  (RFC 3551 §6: 0-95), is skipped; if that leaves no audio media, `parse()` fails with
  `unsupported_encoding`. A dynamic payload type without an rtpmap is `missing_field`.
- L32 (32-bit big-endian linear PCM, rtpmap `L32`) is not an AES67-2023 encoding: AES67
  mandates L16 and L24. It is the RAVENNA extension Merging and others announce, and is
  read case-insensitively and written as a peer of L16 and L24. AM824 is RAVENNA's AES3
  transport (IEC 61883-6 32-bit subframes, as ST 2110-31).
- `ts-refclk` (RFC 7273 §4.8): the domain may be written bare (`:0`, as AES67 devices
  do), as `domain-nmbr=0`, or left out (domain 0); a 2002 `domain-name=` is read as
  domain 0. `to_string()` writes it bare, 0 included, as Dante writes it, with the
  grandmaster as an upper-case EUI-64 in IEEE canonical form. RFC 7273 requires a
  grandmaster or `traceable`; `parse()` tolerates a bare version, keeps it as read
  (`PtpReference::text`) and `to_string()` writes it back so.
- Several `ts-refclk` lines at one level are equivalent clocks (RFC 7273 §4.8), and every
  one is kept, in order: the PTP references in `ReferenceClocks::ptp`, other sources
  (`localmac=`, `ntp=`, ...) as read in `ReferenceClocks::other`, up to
  `ReferenceClocks::MAX` each. AES67-2023 Clause 8.2 asks receivers to evaluate them all.
  Past `MAX`, a PTP reference that can name a clock (a known version with a grandmaster,
  or `traceable`) replaces one that cannot; any other is dropped. A level with
  any `ts-refclk` overrides the session level, so `effective_clocks()` may be empty though
  the session names a PTP clock. With no `ts-refclk` at all, RFC 7273 Section 6 says a
  local clock is assumed: the session names no PTP clock. `to_string()` writes every
  reference, so a parsed session writes back as it was given.
- Versions: the RFC 7273 tokens, `IEEE1588-2019` (SMPTE ST 2110-10 writes it), and
  `IEEE802.1AS-2020`, which is not registered and reads as `IEEE802.1AS-2011` (both are
  gPTP in domain 0); any other `ptp-version-ext` token is `PtpVersion::other`, kept
  unparsed and as read, naming no clock, and never failing `parse()`. IEEE 802.1AS has no domain (AES67-2023 Clause 8.2:
  "no domain indication is supported or necessary"): one written is read as 0, and
  `to_string()` writes none (`ptp=IEEE802.1AS-2011:<EUI-64>`).
- `PtpReference::same_clock()`: one grandmaster in one domain. IEEE 1588-2008, 1588-2019
  and 802.1AS are one PTPv2 clock, so an 802.1AS reference and a 1588 one in domain 0 with
  the same grandmaster match (a boundary clock joins them into one domain, AES67-2023
  Annex D.1); IEEE 1588-2002 (PTPv1) matches only itself, and `traceable` nothing.
- `mediaclk` (RFC 7273 §5): any kind other than `direct` (`sender`, `IEEE1722`) is a
  clock that cannot be derived from PTP, and `effective_media_clock_offset()` is then
  `nullopt`. `MediaClock::kind()` tells `direct`, `sender` and `other` apart
  (`effective_media_clock()` gives the clock in force); `other` is written as `sender`. A media-level clock of either kind overrides a session-level one; a level
  naming both keeps the direct one.
- `clock-domain:PTPv2 <n>` duplicates an IEEE 1588 `PtpReference::domain`; RAVENNA and Dante write it
  beside `ts-refclk` and some receivers look for it, so writers set both. Other clock
  kinds in it, and domains past 127, are not kept; a later line at the same level replaces
  an earlier one. Nothing here reconciles it with `ts-refclk`'s domain: that is the caller's.
- `parse()` reads at most `Session::MAX_MEDIA` (16) audio media sections and fails
  `too_many_media` past them, so a description from the network stays small; sections it
  skips (video, other protocols) are not counted.
- `to_string()` sanitises so its output always parses: line breaks in text fields become
  spaces, an empty username or one with a space is written `-`, and an origin address
  that is not one token as `0.0.0.0`. A unicast connection's TTL is dropped and a
  multicast one always gets one (`DEFAULT_MULTICAST_TTL` when 0), as RFC 8866 §5.7
  requires.
- `a=rtcp:<port>[ IN IP4 <address>]` (RFC 3605) is read at media level into
  `Media::rtcp` and written when set; a sender leaves it unset, since the default (the
  media's address, port + 1) needs no line and IPMX writes none. A port of 0 or another
  address type fails the parse. `a=rtcp-mux` (RFC 5761) only sets `Media::rtcp_mux`:
  AES67 never multiplexes, and the `aes67` receiver then refuses RTCP for the stream
  (`rtcp_multiplexed`) while receiving it. `b=` lines (`b=AS`, `b=RS`, `b=RR`) are
  ignored and never written: the RTCP interval is not derived from them.
- `Media::frames_per_packet()` rounds the ptime to whole frames, since a sender may write
  125 µs as `0.12`.
- Fuzzing: `sdp_parse_fuzzer`, seeded from `fuzz/seeds/sdp_parse_fuzzer/`.

## Further reading

- RFC 8866 — SDP; RFC 7273 — clock source signalling; RFC 4570 — source filters; RFC 3551, RFC 3190 — L16 and L24; RFC 3605 — the RTCP attribute.
- AES67-2023 §8 — session description.
- [`sap`](SAP_MODULE.md) — announcing and discovering these sessions.
