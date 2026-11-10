[← back to module index](README.md)

# sap

RFC 2974 Session Announcement Protocol: packet build and parse, `SapAnnouncer` for the
sessions this host sends, and `SapDirectory` / `SapReceiver` for the sessions others
announce, so a receiver can subscribe to a stream by its session name.

## Overview

Dante and other AES67 devices discover streams by SAP on 239.255.255.255:9875, the
administratively scoped SAP group. `SapAnnouncer` announces each session there on the
first tick and every 30 s after, and sends a deletion when it is withdrawn.
`SapDirectory` is the listening side's session table; it is pure (the caller gives the
time), so it is tested with crafted packets and a fake clock. `SapReceiver` joins the SAP
groups on one interface and feeds the directory, handing each event to a handler. Both
`SapAnnouncer` and `SapReceiver` are `net::Pollable`s for the caller's reactor (control)
thread.

## Key types

- `make_sap_packet()` / `parse_sap_packet()` and `SapMessage` — the RFC 2974 §3 packet: version 1, IPv4 origin, no authentication, encryption or compression.
- `SapAnnouncer` — announces sessions under caller-chosen keys; `announce()`, `withdraw()`, `withdraw_all()`.
- `SapDirectory` — the announced sessions, keyed by `SessionKey`; `handle()` / `handle_packet()` yield `Event`s (`announced`, `deleted`, `timed_out`, `evicted`), `expire()`, `find(name, origin)`.
- `SapReceiver` — the socket and the directory on a reactor; `set_handler()`.
- `SessionKey`, `session_key()`, `origin_line_of()`, `subscription_matches()` — a session's identity and the subscription rule.

## Quick example

```cpp
#include "statusbar/sap/sap_receiver.hpp"

using namespace statusbar;

auto watch(unsigned int interface_index) -> StatusValue<std::unique_ptr<sap::SapReceiver>>
{
    auto receiver = sap::SapReceiver::open(interface_index);
    if (!receiver) {
        return forward_failure(receiver);
    }
    (*receiver)->set_handler([](sap::SapDirectory::Event const& event) {
        if (event.kind == sap::SapDirectory::Kind::announced) {
            // event.entry.session is the new or changed session
        }
    });
    return receiver;  // add it to the reactor
}
```

## Headers

- `statusbar/sap/sap.hpp` — packets, `SapMessage`, `SapAnnouncer`.
- `statusbar/sap/sap_receiver.hpp` — `SessionKey`, `SapDirectory`, `SapReceiver`.

## Dependencies

- **Statusbar modules:** [`sdp`](SDP_MODULE.md), `ip`, `net` (`Pollable`, sockets, group joins), `status`.
- **System / external:** `<vector>`, `<string>`, `<functional>`.

## Notes & caveats

- A changed session is announced again under a new message id hash, with no deletion
  first (RFC 2974 §5); the new hash always differs from the old, and is never 0, which
  RFC 2974 reserves for "no hash".
- `SapAnnouncer::open()` binds its socket to the origin, so its packets leave from the
  address their origin field names, which listeners require.
- `withdraw()` sends the deletion at once, not at the next tick, so a caller shutting
  down may withdraw and close straight away. A deletion carries only the o= line.
- A session is known by its SDP origin (o= username, session id and address: RFC 8866
  §5.2's globally unique identifier). An announcement of a new session, or of a known one
  whose SDP changed (a new o= version, say), is `announced`; an unchanged
  re-announcement only refreshes it.
- `handle_packet()` takes the packet's IP source address and ignores a packet whose SAP
  origin field names another host (RFC 2974 §6: the originating source), or one longer
  than `MAX_PACKET` (1472 octets, a 1500-octet MTU's UDP payload; RFC 2974 §6 recommends
  under 1 KB). `SapReceiver` reads with `recvfrom()` into a buffer one octet longer.
- A deletion counts only from the SAP origin that announced the session, and not for an
  older version than the one announced: a sender that deleted a session before
  announcing its change, and a late copy of that deletion, must not remove it. A deletion
  without an o= line names the announcement by origin and message id hash (RFC 2974 §5);
  an o= line that does not read names nothing.
- A re-announcement likewise counts only from the announcing origin, whatever its
  version: a device whose version counter restarted is read at once.
- Timeout: RFC 2974 §3.1 says ten of the sender's intervals or an hour, whichever is
  more. A source that is gone should show as gone within minutes, so the timeout is fixed,
  by default 15 minutes: two of the RFC's slowest starting intervals (300 s, plus up to a
  third of jitter), and thirty of AES67 devices' usual 30 s.
- Compressed and encrypted packets, payload types other than `application/sdp` (compared
  case-insensitively), and SDP the parser refuses are ignored (`sdp::parse()` reads at most
  `Session::MAX_MEDIA` audio sections). Once `MAX_SESSIONS` are known, a new session
  evicts the one announced least recently, and an origin holding `MAX_SESSIONS_PER_ORIGIN`
  (128) evicts its own, so one host cannot flood out the others' sessions. The announced
  `Event` carries the evicted entry in `evicted`, and `SapReceiver` hands it to its handler
  first as a `Kind::evicted` event.
- `SapReceiver` joins the administratively scoped group (AES67's) first and must succeed
  there; the organisation-local and global groups are best effort. The handler runs
  inside `on_ready()` and `tick()`: it may read `directory()` but must not destroy the
  receiver or replace the handler.
- Fuzzing: `sap_directory_fuzzer` feeds packets through `SapDirectory::handle_packet()` (with
  `sdp::parse()`, `origin_line_of()` and `sdp::to_string()` on what parses); its seeds are in
  `fuzz/seeds/sap_directory_fuzzer/` (`sap_directory_fuzzer fuzz/seeds/sap_directory_fuzzer`).

## Further reading

- RFC 2974 §3, §3.1, §5, §6.
- AES67-2023 §9 — discovery.
- [`sdp`](SDP_MODULE.md) — the payload.
