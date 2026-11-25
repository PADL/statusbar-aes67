[← back to docs index](README.md)

# AES67 transports

The contract between the `aes67` module's streams and their transports, `UdpTransport`, and
streams under SRP; `XdpTransport` and launch time are in [AES67_XDP.md](AES67_XDP.md), the
module overview in [AES67_MODULE.md](AES67_MODULE.md).

## Transport contract

- The control thread (one thread) calls `open_receive()` / `open_send()` and destroys
  ports; these may allocate and make syscalls. The real-time thread calls
  `ReceivePort::receive()`, `SendPort::send_rendered()`, `flush()`,
  `begin_tick()` and `end_tick()`: no allocation, no blocking, no logging.
- Opening and destroying ports is safe while the real-time thread ticks the transport. A
  transport that shares state between ports (XDP) hands port changes to the real-time
  thread through queues, applies them in `begin_tick()`, and frees a closed port's state
  only after the real-time thread has let it go.
- Do not destroy a port while the real-time thread may still call it: swap it out between
  ticks and retire it first. The transport outlives every port it opened (asserted in its
  destructor), and the real-time thread stops ticking it before it is destroyed.
- A datagram's `payload` is the transport's until the next `receive()` on the port or the
  next `begin_tick()`, whichever is first.
- `send_rendered()` reserves the buffer first; when it cannot, `render` is not called. When `render` returns false, or the packet is refused after it
  ran (the XDP TX ring is full), the buffer is released unsent: a batch slot is not
  counted, a UMEM frame goes back to the pool, or, refused by the ring, is freed. A
  transport calls `render` at most once per packet, because the sender pulls its frames
  from the provider inside it.
- `open_rtcp()` and every `RtcpPort` call belong to the control or reactor thread, never
  the real-time one: RTCP is not real-time traffic. A port whose `fd()` is -1 has no
  descriptor that wakes for all its datagrams, and its owner calls `receive()` each
  reactor cycle. The base `Transport` refuses `open_rtcp()` (`operation_not_supported`),
  so test transports need not implement it.
- RTCP DSCP (AES67-2023 Clause 6.2): a sender marks its RTCP as its RTP stream, a receiver
  as it would mark a similar stream. `RtcpOptions::dscp` is that value; the caller passes
  the stream's (or would-be stream's) `SendOptions::dscp`, not the RTCP class of its own.
- `Receiver` and `Sender` open their ports with `open_receive_stream(ReceivePlan)` and
  `open_send_stream(SendPlan)`: the whole stream, so a transport may choose how to carry it.
  The defaults call `open_receive_from()` (a source-specific group with a source to join
  for), `open_receive()` and `open_send()`; `UdpTransport` and `XdpTransport` keep them.

## Frame ports

A port may packetise and depacketise the stream itself, beside the network, and take or give
frames at sample indices: `ReceivePort::frames()` returns its `FrameReceive`, `SendPort::frames()`
its `FrameSend` (both null for a packet port, and constant for the port's life).

- A `FrameReceive` admits and places packets by its `ReceivePlan` with the `Depacketizer`'s
  rules and the plan's offset. `Receiver::drain()` asks it for the frames it placed
  (`drain(now_index, handler)`), its counters and its `ReceiveState`, and passes on each change
  of the offset (`set_offset()`); a sender-clocked plan's port forgets its offset on a new source
  (`FrameDrain::offset_forgotten`), as the packet path does. There is no due-tick gate. A
  `Receiver` without a `FrameHandler` hands the port one that drops the frames.
- A `FrameSend` sends the `SendRun` it is given (SSRC, first sequence, first index, send offset)
  on its `SendPlan`'s grid with a `PacketSchedule`. `Sender::tick()` keeps its own schedule:
  each packet due is written into the port (`writable()`, in pieces where the port's storage
  wraps, then `commit()`), counted as the packet path counts it, and the port sends it. A
  run's first packet is placed `start_lead_frames()` past the newest due packet (held as for a
  report), `pause()` stops the run, a hold that ends before that packet restarts it from the
  newest due packet, and a changed send offset is passed on. A packet the port had no room for is
  dropped, and the run leaves it out (its sequence number spent), as a packet port does. The
  `SendState` an RTCP session reads comes from the port's `progress()` once that names the run
  (`SendProgress::ssrc` and `first_index`); until the port has taken it, the run's first packet
  is published as not yet sent.
- Both are called on the real-time thread and neither may block or make a system call there.
  `start()`, `stop()`, `set_send_offset()` and `set_offset()` only record the change: the
  implementation publishes it (through `itc`) to a thread off the tick, which applies it to
  whatever does the packet work. The tick never applies them itself.
- `LoopTransport` in `aes67_test_support.hpp` has a frame port of each kind (`frame_ports`),
  built from `Depacketizer` and `PacketSchedule`; the tests show the frames, counters, packets
  and their timestamps equal the packet path's for the same input.

## UdpTransport

- The one transport that carries unicast (AES67-2023 Clause 6.1.1), both ways: every
  stream has its own socket, a group's joined and a unicast address's bound, so groups and
  unicast addresses never share one. `XdpTransport` refuses unicast.
- A receiver's socket is bound to its destination address itself, so a group's socket
  sees only that group and a unicast receiver's specific bind wins its address:port over
  the group sockets sharing the port; `IP_PKTINFO` checks each datagram's destination as
  well. One unicast receiver per address and port.
- Group sockets, RTP and RTCP alike (`open_receive_socket()`), are scoped to the
  transport's interface, so two transports on two NICs can receive the same group. Linux
  delivers a group's datagrams to every socket bound to it, whichever interface joined
  it, unless `IP_MULTICAST_ALL` is 0; with it 0, a socket gets only the groups it joined,
  on the interfaces it joined them on. Each socket sets it to 0, and `receive()` also
  drops a datagram whose `IP_PKTINFO` `ipi_ifindex` is not the transport's interface (one
  compare on the ancillary data already read). `SO_BINDTODEVICE` would scope the socket
  too, but needs `CAP_NET_RAW` before Linux 5.7 and also pins the socket's sends.
  `aes67_veth_test` proves it on two veth pairs; it needs root, or runs unprivileged in a
  user and network namespace:
  `unshare -rn sh -c 'ip link set lo up; build/statusbar_test padl/aes67/aes67_veth_test'`.
- `receive_refusals()` counts a truncated datagram (longer than `MAX_DATAGRAM`) as
  `malformed`; one `IP_PKTINFO` puts on another address or interface (another group's on a
  shared port) is skipped uncounted, and the receiver checks the rest (`Counters::drops`).
- A sender's socket is unconnected and every message names the destination, so a unicast
  receiver's ICMP port-unreachable never fails a send (`open_send_socket()`). Multicast goes
  out of the interface with the configured TTL; unicast is bound to the interface's address
  and leaves by it (`net::set_unicast_send()`: `IP_UNICAST_IF`, unprivileged, unlike
  `SO_BINDTODEVICE`), whatever the routing table prefers, with `SendOptions::ttl` and socket
  priority 0, so the DSCP's priority (Linux's `rt_tos2priority()`: AF41 is 2) never puts it
  in an SR class's queue (AES67-2023 Annex C.1.2). Both carry the sender's DSCP (`SendOptions::dscp`, from
  `Sender::set_dscp()`, by default `rtp::FrameAddressing::DSCP_MEDIA`, 34). A tick's packets are written straight
  into the `SEND_BATCH` slots of the sender's `sendmmsg()` batch.
- An RTCP port is one socket for both directions. For a group it is bound to the group at
  the RTCP port, joined, and sends there with loopback on, so a session receives its own
  reports and must skip its own SSRC; for a unicast peer it is bound to `RtcpOptions::local`
  (by default this address at the destination port), without `SO_REUSEADDR`, leaves as its
  stream's socket does (by the interface, with `RtcpOptions::ttl`, at socket priority 0),
  and sends to the peer: a second port on the same unicast address and port is refused
  (`address_in_use`) rather than splitting the reports between the two.

## Streams under SRP

- AES67-2023 Annex C.1.1 carries a stream as an IEEE 802.1Q Clause 35 reservation. `srp_tspec(format,
  class)` is its TSpec (Clause 35.2.2.8.4): MaxFrameSize counts the IPv4, UDP and RTP headers, the
  payload and one spare octet, no Ethernet overhead (Annex C.1.1.1); MaxIntervalFrames the packets
  that can start in one class interval (125 us for class A, 250 us for B, Clause 34.4).
- A packet time longer than the interval would reserve a whole frame every interval for one sent
  each packet time. `srp_frames_per_packet(rate, class)` is the fewest frames whose packet time
  reaches the interval, AES67-2023 Table 2's "125 microseconds" in class A (6 at 48 kHz, exactly
  125 us; 6 at 44.1 kHz, 136 us, one frame per interval; 12 at 96 kHz) and "250 microseconds" in
  class B (12, 12, 24), for which MaxIntervalFrames is 1. Which packet times a caller sends or
  accepts under SRP is its own policy.
- A tagged stream: `SendOptions::vlan` opens a port tagged; `SendPort::set_vlan()` switches it between
  tagged and untagged from the next frame (`XdpTransport`: one atomic word, wait-free for the
  tick, read per frame), and the sender's run, sequence and timestamps go on. `UdpTransport` cannot tag and
  refuses a tag. `Sender::set_vlan()` keeps the tag for its next `configure()`.
- Reservations filter by destination MAC (Clause 35.2.2.8.3): `multicast_mac_aliases()` says when two
  groups share one.
