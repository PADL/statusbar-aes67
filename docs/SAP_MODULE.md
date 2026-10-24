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
- `SapDirectory` — the announced sessions, keyed by `SessionKey`; `handle()` / `handle_packet()` yield `Event`s (`announced`, `deleted`, `timed_out`), `expire()`, `find(name, origin)`.
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

- A changed session gets a new message id hash, so listeners take it as new; a hash is
  never 0, which RFC 2974 reserves for "no hash".
- `withdraw()` sends the deletion at once, not at the next tick, so a caller shutting
  down may withdraw and close straight away. A deletion carries only the o= line.
- A session is known by its SDP origin (o= username, session id and address: RFC 8866
  §5.2's globally unique identifier). An announcement of a new session, or of a known one
  whose SDP changed (a new o= version, say), is `announced`; an unchanged
  re-announcement only refreshes it.
- A deletion counts only from the SAP origin that announced the session, and not for an
  older version than the one announced: a sender that changes a session deletes the old
  one first, and a late copy of that deletion must not remove the new one. A deletion
  without an o= line names the announcement by origin and message id hash (RFC 2974 §5);
  an o= line that does not read names nothing.
- A re-announcement likewise counts only from the announcing origin, and an older version
  only after a second (a device whose version counter restarted, not a late copy).
- Timeout: RFC 2974 §3.1 says ten of the sender's intervals or an hour, whichever is
  more. AES67 devices announce far more often than the RFC's 300 s minimum and a source
  that is gone should show as gone within minutes, so the default is ten 30 s intervals.
  A session re-announced at longer intervals gets ten of those (the longer of its last
  two gaps), up to the hour (`MAX_TIMEOUT_NS`); one heard only once gets twice the
  timeout.
- Compressed and encrypted packets, payload types other than `application/sdp` (compared
  case-insensitively), and SDP the parser refuses are ignored. Once `MAX_SESSIONS` are
  known, new ones are ignored until some time out.
- `SapReceiver` joins the administratively scoped group (AES67's) first and must succeed
  there; the organisation-local and global groups are best effort. The handler runs
  inside `on_ready()` and `tick()`: it may read `directory()` but must not destroy the
  receiver or replace the handler.

## Further reading

- RFC 2974 §3, §3.1, §5.
- AES67-2018 §9 — discovery.
- [`sdp`](SDP_MODULE.md) — the payload.
