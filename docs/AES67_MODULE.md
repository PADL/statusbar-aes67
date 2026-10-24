[← back to module index](README.md)

# aes67

AES67 streams: `Receiver` and `Sender`, each one RTP stream described by SDP with frames
addressed by PTP sample index, over a `Transport`: `UdpTransport` (sockets, any host) or
`XdpTransport` (AF_XDP raw frames on one NIC queue).

## Overview

A `Receiver` is configured from an SDP media section and, each tick, drains what its
transport received, validates source and sequence, turns each RTP timestamp into a
sample index with `mediaclock::PtpMediaClock`, and hands the payload to its
`FrameHandler`. A `Sender` asks its `FrameProvider` for the frames of every packet due on
the sample-index grid, renders them straight into the transport's transmit buffer, and
describes its stream in SDP (`session()`) for SAP and control.

Both run on one real-time thread that ticks every 125 µs, bracketed by the transport's
`begin_tick()` (before any receiver drains) and `end_tick()` (after every sender has
sent). Configuration happens on a control thread.

## Key types

- `StreamFormat` — encoding, channels, rate and frames per packet (the ptime).
- `FrameHandler` / `FrameProvider` — the real-time callbacks, `sg14::inplace_function`s with 64 bytes of capture; `Supply` is what a provider gave.
- `Receiver` — `configure(session, media_index)`, `drain(now_index)`, counters, state, learnt SSRC.
- `Sender` — `configure(destination | session)`, `tick(now_index)`, `session()`, read delay, packet phase, session id, grandmaster, name, `continue_from()`.
- `Destination`, `Grandmaster`, `State`, `Counters` — what a stream is sent to, its ts-refclk, its state and its figures.
- `Transport`, `ReceivePort`, `SendPort`, `PayloadRenderer` — the transport contract; `UdpTransport`, `XdpTransport` (`XdpTransportOptions`) implement it.
- `Aes67Error` — `enum class`, category `statusbar.aes67`.

## Quick example

```cpp
#include "statusbar/aes67/aes67_receiver.hpp"
#include "statusbar/aes67/aes67_udp_transport.hpp"

using namespace statusbar;

auto receive(std::string_view interface, sdp::Session const& session) -> Status
{
    auto transport = aes67::UdpTransport::open(interface);
    if (!transport) {
        return forward_failure(transport);
    }
    aes67::StreamFormat const format{.encoding = sdp::Encoding::l24, .channels = 2};
    aes67::Receiver receiver{format, [](uint64_t, uint32_t, std::span<uint8_t const>) {}, **transport};
    if (auto configured = receiver.configure(session); !configured) {
        return configured;
    }
    // On the real-time thread, every 125 us tick:
    //   (*transport)->begin_tick(); receiver.drain(now_index); (*transport)->end_tick();
    return success();
}
```

## Headers

- `statusbar/aes67/aes67_stream.hpp` — `StreamFormat`, callbacks, `Destination`, `Grandmaster`, `State`, `Counters`.
- `statusbar/aes67/aes67_receiver.hpp` — `Receiver`.
- `statusbar/aes67/aes67_sender.hpp` — `Sender`, `parse_grandmaster()`.
- `statusbar/aes67/aes67_transport.hpp` — `Transport`, `ReceivePort`, `SendPort`, `PayloadRenderer`, `Endpoint`, `Datagram`, `SendOptions`.
- `statusbar/aes67/aes67_udp_transport.hpp` — `UdpTransport`.
- `statusbar/aes67/aes67_xdp_transport.hpp` — `XdpTransport`, `XdpTransportOptions`.
- `statusbar/aes67/aes67_socket.hpp` — `InterfaceInfo`, `find_interface()`, sockaddr conversion.
- `statusbar/aes67/aes67_error.hpp` — `Aes67Error`.

## Dependencies

- **Statusbar modules:** [`mediaclock`](MEDIACLOCK_MODULE.md), [`rtp`](RTP_MODULE.md), [`sdp`](SDP_MODULE.md), `buffer`, `ieee`, `ip`, `itc`, `net` (sockets, `XdpContext`, `CompiledRule`), `status`.
- **System / external:** Linux `recvmmsg()` / `sendmmsg()`, `IP_PKTINFO`; libxdp and an AF_XDP-capable NIC for `XdpTransport`.

## Notes & caveats

### Transport contract

- The control thread (one thread) calls `open_receive()` / `open_send()` and destroys
  ports; these may allocate and make syscalls. The real-time thread calls
  `ReceivePort::receive()`, `SendPort::send()`, `send_rendered()`, `flush()`,
  `begin_tick()` and `end_tick()`: no allocation, no blocking, no logging.
- Opening and destroying ports is safe while the real-time thread ticks the transport. A
  transport that shares state between ports (XDP) hands port changes to the real-time
  thread through queues, applies them in `begin_tick()`, and frees a closed port's state
  only after the real-time thread has let it go.
- Do not destroy a port while the real-time thread may still call it: swap it out between
  ticks and retire it first. The transport outlives every port it opened (asserted in its
  destructor), and the real-time thread stops ticking it before it is destroyed.
- `send_rendered()` reserves the buffer first; when it cannot (as `send()` would fail),
  `render` is not called. When `render` returns false, or the packet is refused after it
  ran (the XDP TX ring is full), the buffer is released unsent: a batch slot is not
  counted, a UMEM frame goes back to the pool, or, refused by the ring, is freed. A
  transport calls `render` at most once per packet, because the sender pulls its frames
  from the provider inside it.

### Sender

- Packets lie on a grid of `frames_per_packet`: packet k carries indices
  `[k * fpp + phase, (k + 1) * fpp + phase)`. Its RTP timestamp is
  `PtpMediaClock::rtp_timestamp(first index, 0)`, so the SDP says `mediaclk:direct=0` and
  receivers on the same grandmaster place every sample exactly.
- The read delay (the link offset) gates sending: a packet goes out on the first tick at
  which its last frame's index is `read_delay()` or more behind the current index. A
  network source delivers frames after their index: an AES67 packet `[n, n + 48)` arrives
  no earlier than `(n + 48) / fs` plus its latency. `default_read_delay()` is one 125 µs
  tick, for a provider whose frames are there by their index (generated, captured
  locally, or placed ahead of time); a receiver then sees the packet about a packet time
  after its RTP timestamp (1.125 ms for 1 ms packets at 48 kHz). The lower the delay, the
  more of a receiver's latency budget is left (a Dante receiver at its 1 ms setting is
  already late with 1 ms packets), but a provider whose frames land later than that has
  them counted lost. `relay_read_delay()`, a packet plus two ticks, is for a provider fed
  from the network (an AES67 receiver's frames). A changed delay takes effect at the next
  tick: a longer one waits, a shorter one catches up, up to `MAX_BURST` packets.
- Send phase stagger: with every 1 ms output on phase 0, all of them send on the same
  tick in eight, and that burst, not the mean load, sets the ceiling. Give the kth sender
  `stagger_phase(format, k)`, (k mod 8) ticks modulo the packet (0, 6, ... 42 frames for
  1 ms at 48 kHz), before `configure()`. The phase adds no latency and leaves the
  timestamps on the index grid.
- The SSRC is random per `configure()`, the sequence starts at a random value and counts
  up, and the marker is never set.
- Frames the provider does not have (`Supply::missing`) are sent as silence and counted
  lost; with no provider (`Supply::none`) silence is sent and nothing is counted. More
  than `MAX_BURST` packets behind, the sender skips ahead and counts the skipped frames
  lost. After a backward step of the clock it sends nothing until now catches up with
  what it has sent, so no index goes out twice, unless the step exceeds `MAX_BURST`
  packets, when it resynchronises.
- Payloads are rendered in place: `tick()` hands the provider the payload span of the
  transport's next TX buffer through `SendPort::send_rendered()`, so the frames are copied
  once, provider to wire buffer. When the transport has no buffer to give (counted
  dropped), the frames are pulled into a staging buffer instead, so a missing pull is
  counted lost all the same; when the transport refuses the packet after rendering, the
  frames were already pulled and the packet is counted dropped without a second pull.
- A sender is in `State::error` while its transport reports `send_error()`, checked every
  tick, and returns to ready when it clears.
- `session()` gives origin the transport's interface address, the group with its TTL, the
  encoding, channels, rate and ptime, `ts-refclk:ptp=IEEE1588-2008:<EUI-64>:<domain>` and
  `clock-domain:PTPv2 <domain>` from `set_grandmaster()` (always PTPv2, the domain always
  written), `mediaclk:direct=0`, a source filter naming this host for a multicast group,
  and `a=recvonly`, as AES67's examples and Dante write it.
- Session id and version: a receiver (Dante Controller, say) keys a session on the o=
  session id and origin, so a new id each start leaves stale sessions behind. The id is
  stable across restarts: `set_session_id()`, or by default `stable_session_id()`
  (FNV-1a) of the origin address and the name, so two senders on one host with the same
  name need ids of their own. The version starts at the wall clock's seconds (RFC 8866's
  suggestion, so it grows across restarts too) and goes up by one with each
  `configure()`, grandmaster change and rename. Ids and versions are kept to 63 bits.
  `continue_from()` carries the id and version (and the read delay, grandmaster and name)
  to a replacement sender, so its next `configure()` announces the same session one
  version on.

### Receiver

- `configure()` takes the media's group and port, payload type, encoding, channels and
  rate, an RFC 4570 source filter if any, and the RFC 7273 direct media clock offset. It
  prefers the offered format with the receiver's encoding (`Media::prefer()`), else the
  primary one; the encoding must be the receiver's (`encoding_mismatch`), and channels and
  rate too (`format_mismatch`). The ptime need not match: any whole number of frames is
  taken. A stream without a direct media clock is refused (`not_ptp_locked`): its
  timestamps are not PTP time.
- Packets of the stream's payload type are accepted. The first one's SSRC is learnt and
  other sources dropped until the learnt one has been silent for a second. The sequence is
  tracked with `rtp::RtpSequence`; a new source's probation packets are handed on too, and
  only an unconfirmed jump of a validated source is dropped. The payload goes to the
  `FrameHandler` as is.
- Due-tick gate: over a queuing transport (UDP), `drain()` skips the port until a tick
  before the next packet can have been sent (after `[i, i + n)`, the next is sent no
  earlier than index `i + 2n`), then asks every tick until one comes. It never waits more
  than a packet time and a tick, and asks every tick before the first packet, after a
  backlog, and after a clock step back. At 1 ms that is about three receive calls in eight
  ticks instead of eight. The gate relies on the sender's timestamps: a sender stamping
  ahead of capture has its packets read up to a packet time late. Over XDP
  (`drain_every_tick()`), a port keeps datagrams only for the tick they arrived in, so
  there is no gate. `polls()` and `skipped_polls()` count the calls made and skipped.
- Counters: `packets` and `frames` handed on; `lost`, frames in sequence gaps not (yet)
  filled; `late`, frames that arrived out of order (after a later packet), still handed
  on; `dropped`, packets refused (malformed, wrong payload type, source or size,
  duplicates, or too old to place).

### UdpTransport

- A receiver's socket is bound to its destination address itself, so a group's socket
  sees only that group and a unicast receiver's specific bind wins its address:port over
  the group sockets sharing the port; `IP_PKTINFO` checks each datagram's destination as
  well. One unicast receiver per address and port.
- A sender's socket is unconnected and every message names the destination, so a unicast
  receiver's ICMP port-unreachable never fails a send. Multicast goes out of the
  interface with the configured TTL and DSCP 46. A tick's packets are written straight
  into the `SEND_BATCH` slots of the sender's `sendmmsg()` batch.

### XdpTransport

- The XDP program redirects to the socket the untagged IPv4 (IHL 5) UDP frames whose
  destination is exactly one of the joined groups (the rule matches the group's MAC and
  the address's first two octets, which together are the whole address) and passes
  everything else to the kernel. `begin_tick()` drains the RX ring once and hands each
  frame, parsed with `rtp::parse_rtp_frame()`, to the ports for its destination group and
  port; senders write whole frames into UMEM and `end_tick()` kicks the NIC.
- Group membership is still the kernel's: each receive port holds a plain UDP socket
  joined to its group, so IGMP reports go out and the NIC's multicast filter keeps the
  group.
- `begin_tick()` is the only place the port list and the installed rules change: it takes
  new ports from a bounded queue, drops closed ones (handing their state back on a retire
  queue, freed on the control thread's next `open_*()` or port destruction, or by the
  destructor), and reinstalls the rules when the set changed. That is a BPF map update, a
  syscall on the real-time thread, only when receivers come or go.
- TX pool: senders keep `TX_POOL` UMEM frames back, topped up each tick before the fill
  ring is; those are never offered to the kernel. A send takes a frame from the pool,
  writes it and commits it to the TX ring; a frame that does not fit, or whose render is
  abandoned, goes back to the pool (never to the kernel), and one the full TX ring refuses
  is freed to the UMEM. The rest of the UMEM backs the fill ring (`FILL_RING` entries) and
  the frames in flight.
- Refused kicks: each `end_tick()` kick the kernel refuses is counted with its errno
  (`tx_flush_failures()`, `tx_flush_error()`); `TX_FAILING_AFTER` (8, 1 ms of ticks) in a
  row set `send_error()`, which every sender reports as its error, until a kick goes
  through.
- Counters: `socket_stats()` is the socket's `net::XdpSocketStats` as of the last
  `end_tick()` (frames received and sent, ring-full, UMEM-exhausted, kicks made, skipped
  and transient), and `tx_pool_empty_count()` the sends that found the TX pool empty;
  both may be read from any thread.
- Limits: multicast destinations only, both ways (a unicast receiver would need a rule
  stealing the host's UDP, a unicast sender ARP); at most `MAX_GROUPS` distinct groups
  (the BPF program's TCAM holds 16 rules); a joined group's frames to ports no receiver
  here listens on do not reach the kernel (the rule cannot see the UDP port), so other
  listeners on the same group miss them; untagged frames only; one queue (`QUEUE`, 0), so the
  NIC must deliver the groups there.
- RSS: a NIC with several RX queues hashes UDP flows over all of them, so `open()` steers
  every RSS bucket to `QUEUE` (`XdpTransportOptions::steer_rss`, on by default;
  `net::set_rss_indirection_all`, `CAP_NET_ADMIN`) and `rss_status()` says whether it could.
  A NIC without RSS reports `operation_not_supported` and needs one combined channel or a
  flow-steering rule instead. The table is left steered at close.
- AF_XDP zero-copy, attach and pinning behaviour is hardware-dependent and verified on the
  bench box, not in unit tests; `aes67_xdp_test` covers the frame, pool and health logic.

## Further reading

- AES67-2018 — interoperability requirements for audio over IP.
- RFC 3550 — RTP; RFC 7273 — clock source signalling; RFC 4570 — source filters.
- [`mediaclock`](MEDIACLOCK_MODULE.md), [`rtp`](RTP_MODULE.md), [`sdp`](SDP_MODULE.md), [`sap`](SAP_MODULE.md).
- statusbar-core `net` module — `XdpContext`, `CompiledRule`, sockets.
