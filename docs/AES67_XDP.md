[← back to docs index](README.md)

# AES67 over AF_XDP

How the `aes67` module's `XdpTransport` receives and sends on one NIC queue, and the launch
times it gives its frames; the transport contract is in [AES67_TRANSPORTS.md](AES67_TRANSPORTS.md),
the module overview in [AES67_MODULE.md](AES67_MODULE.md).

## XdpTransport

- The XDP program redirects to the socket the untagged IPv4 UDP frames, options or none
  (the rules pin the version, not the IHL), whose destination is exactly one of the joined groups (the rule matches the group's MAC and
  the address's first two octets, which together are the whole address) and passes
  everything else to the kernel. With `set_receive_vid(vid)` one more rule, after the
  groups' (so setting or clearing the VID rewrites only the tail), redirects every IPv4
  group's UDP frames tagged with `vid`, any PCP (`tagged_groups_rule()`: MAC 01:00:5e with
  bit 24 clear, RFC 1112 Section 6.4); the ports accept their groups' and count the rest
  `unmatched`, and the kernel no longer sees tagged multicast on that VID. `begin_tick()`
  drains the RX ring once and hands each frame, parsed with `rtp::parse_rtp_frame()`, to the ports for its destination group and
  port; senders write whole frames into UMEM and `end_tick()` kicks the NIC.
- Group membership is still the kernel's: each receive port holds an unbound UDP socket
  joined to its group (`net::open_group_membership()`), so IGMP reports go out and the
  NIC's multicast filter keeps the group. Nothing reads it, and the tick makes no system
  call for receive ports beyond the RX ring. What the program passes is not received: a
  priority-tagged frame (VID 0, see Limits), a frame tagged with a VID other than
  `set_receive_vid()`'s, or a frame on a queue other than `queue_id()` (RSS below).
- `begin_tick()` is the only place the port list and the installed rules change: it adopts
  new ports from a bounded queue, drops closed ones (handing their state back on a retire
  queue, freed on the control thread's next `open_*()` or port destruction, or by the
  destructor), and reinstalls the rules when the set changed. That is a BPF map update, a
  syscall on the real-time thread, only when receivers come or go.
- TX pool: senders keep `TX_POOL` UMEM frames back, topped up each tick before the fill
  ring is; those are never offered to the kernel. A send claims a frame from the pool,
  writes it and commits it to the TX ring; a frame that does not fit, or whose render is
  abandoned, goes back to the pool (never to the kernel), and one the full TX ring refuses
  is freed to the UMEM. The rest of the UMEM backs the fill ring (`FILL_RING` entries) and
  the frames in flight.
- UMEM frames are `net::XdpConfig`'s default 4096 octets. The kernel keeps 256 octets of XDP
  headroom in each, and igb sizes its receive buffers in 1 KiB units below what is left:
  2048-octet frames give 1 KiB buffers, and igb hands the socket a longer frame cut to its
  first KiB, which `parse_udp_frame()` refuses (an 8-channel L24 1 ms packet is 1206
  octets). 4096 leaves 3 KiB buffers. Check a NIC's queue with `ethtool -d` (SRRCTL). The
  UMEM is then 8 MiB locked (`UMEM_FRAMES` × 4096), a default `RLIMIT_MEMLOCK`'s whole 8 MiB:
  the process needs `CAP_IPC_LOCK` or a raised limit.
- Refused kicks: each `end_tick()` kick the kernel refuses is counted with its errno
  (`tx_flush_failures()`, `tx_flush_error()`); `TX_FAILING_AFTER` (8, 1 ms of ticks) in a
  row set `send_error()`, which every sender reports as its error, until a kick goes
  through.
- Counters: `socket_stats()` is the socket's `net::XdpSocketStats` as of the last
  `end_tick()` (frames received and sent, ring-full, UMEM-exhausted, kicks made, skipped
  and transient), and `tx_pool_empty_count()` the sends that found the TX pool empty;
  both may be read from any thread.
- Refusals: `receive_refusals()` counts the redirected frames no receive port kept, by
  `Refusal`: `malformed` (`parse_udp_frame()` refused it: cut short, as a NIC buffer
  smaller than the frame leaves it, or lengths past its end), `unmatched` (a joined
  group's frame to a port no receive or RTCP port has, such as RTCP while no session
  runs), `not_rtp` (to a receive port, but not RTP) and `port_full` (every port for it already
  held `PORT_BATCH` datagrams this tick). A frame counts once, and only when no port kept it;
  the stream's own RTCP on its RTP port (`is_multiplexed_rtcp()`, RFC 5761 Section 4) is
  ignored uncounted, by the transport and by the receiver alike. Relaxed atomics, written by
  `begin_tick()`, read from any thread. `dispatch_frame()` is that per-frame step.
- Limits: multicast destinations only, both ways (a unicast receiver would need a rule
  stealing the host's UDP, a unicast sender ARP): unicast is `UdpTransport`'s alone, and
  nothing on the AF_XDP path tests for it; at most `MAX_GROUPS` distinct groups;
  `MAX_RULES` rules, the groups' and the tagged one, as far as core's TCAM
  (`net::tcam_default_capacity`) holds them: the tagged rule keeps its slot, and a group
  past the room left is dropped and reported by `rules_status()`; a joined group's frames to ports no
  receiver here listens on do not reach the kernel (the rule cannot see the UDP port), so
  other listeners on the same group miss them; tagged frames with one VID only; one queue
  (`Options::queue`, by default `QUEUE`, 0), so the NIC must deliver the groups there, and a NIC that strips VLAN tags
  (`rxvlan`) delivers them untagged.
- Priority-tagged frames (VID 0, untagged by IEEE 802.1Q) are not received, by design: with
  the tag the destination address lies past the 32-octet window, so no rule can classify
  them by group, and no sender known on AES67 networks sends them. A TCAM of 64 rules (a
  second rule per group, keyed on the MAC) is the path if one appears.
- RSS: a NIC with several RX queues hashes UDP flows over all of them, so `open()` always
  steers every RSS bucket to its queue (`net::RssSteering`, `CAP_NET_ADMIN`), and
  fails with `Aes67Error::rss_not_steered` when it cannot (`rss_verdict()`): a spread table
  would lose groups silently. It steers before the program attaches (on igb an attach resets
  the adapter); the write is one ioctl, so a refusal leaves the table as it was, and a socket
  that then fails to open puts the table back as the guard goes (unless the NIC's table size
  changed meanwhile). `steer_rss()` is that step alone (`net::set_rss_indirection_all`). A queue other
  than 0 (an SR class queue a credit-based shaper paces) receives the same steering; traffic the
  NIC does not hash (non-IP) stays on its default queue and the kernel.
  A NIC without RSS reports `operation_not_supported` in `rss_status()` and opens; it needs one
  combined channel or a flow-steering rule. The table is left steered at close (the guard is
  released once the socket opens). `UdpTransport`
  never touches RSS: the kernel's sockets receive on every queue.
- RTCP: the program steals every frame to a redirected group, whatever its port, so an
  RTCP port's kernel socket would miss reports to a group a receiver here has joined.
  `begin_tick()` therefore parses each frame as UDP (`rtp::parse_udp_frame()`; the RTP
  parse is only for receive ports) and copies one for an open RTCP port's group and port
  into that port's `RtcpPipe` (`DEPTH` datagrams of at most `MAX_DATAGRAM`, 1500, bytes,
  as a kernel socket keeps; the rest are dropped and counted in `RtcpPort::dropped()`),
  and the frame is not held. `receive()`
  drains the pipe and the kernel socket, which gets the group's reports while no rule
  redirects it and sends every report (the program steals RX only). RTCP ports do not
  add rules. `fd()` is -1.
- AF_XDP zero-copy, attach and pinning behaviour is hardware-dependent and verified on the
  bench box, not in unit tests; `aes67_xdp_test` covers the frame, pool, health and launch-time logic.

## Launch time

- A sender hands each packet over with its departure (`SendPort::send_rendered_at`): the time of
  the tick that sends it, as the caller gives `Sender::tick(now_index, tick_ns)`, in the ns the
  sending NIC's PHC reads (PTP time; a member interface's domain time). The sender reconstructs
  nothing: a tick without a time sends untimed, a stalled sender's burst departs at the tick that
  catches up. The RTP timestamp is the frames' index (RFC 3550 Section 5.1, capture time) and does
  not move with the departure or the lead.
- `XdpTransport` opened with `XdpTransportOptions::launch_time` (the host found that the driver
  honours it, `net::netdev::launch_time_supported`; a build, kernel or NIC without it opens without
  and records `launch_time_error()`) gives every frame on the socket a launch time (`LaunchClock`),
  in the NIC's PHC time: now (`net::phc_now_ns()` of the clock the host gives `set_phc_clock()`;
  without one, 0: untimed, so every frame goes at once and a lead has no effect) until
  `set_launch_lead_ns()` puts a lead in force, then departure plus lead, never earlier than the last
  time committed (`net::LaunchOrder`: igc's queue is a FIFO; the last time a second or more ahead of
  now is a clock step; a time already past at commit, an overrunning tick, goes at once and is
  counted, as are holds and steps, in `launch_clock().order()`); `clear_launch_lead()` is now again.
  An untimed send (a tick without a time) goes at now. RTCP and SAP go by kernel sockets,
  never through the socket.
- The lead moves the wire time only: the packet leaves at departure plus lead, free of the sending
  CPU's jitter, while its timestamp and the receiver's placement are unchanged, so launch time
  changes no latency; it lets a receiver's link offset cover the lead plus the network transit
  instead of the sender's jitter as well (AES67-2023 Clause 7.4). A lead of 0 would be past when
  the tick commits the frame (the NIC sends at once, as without launch time), so the lead is a
  whole tick or more.
- The NIC honours the time only with the socket's queue in launch-time mode:
  `XdpTransport::settle_launch_time(interface, mode)` lays core's mqprio root
  (`LAUNCH_TIME_QDISC_MAJOR`, every priority mapped away from `QUEUE`) with an offloaded etf on the
  socket's queue (`net::tc::settle_launch_time_queue`). On igc the first etf and the removal of the
  last each reset the adapter (its PHC time is kept across the reset, `igc_ptp_time_restore`), so a
  host settles it once, before anything streams, and pauses launch time by clearing the lead, not by
  removing the etf. Stock igc estimates its cycle from CLOCK_TAI, so on it that holds only while the
  system clock follows the PHC. `unsupported` (igb) looks nothing up and sends nothing. None of this
  has run on an igc yet.
- `launch_time_queue(tx_queues)` also puts PTP event traffic on `PTP_QUEUE` (2) with 4 TX queues or
  more (`net::tc::PTP_PRIORITY`, ptp4l's `socket_priority 7`), queue 1 idle and the rest on queue 3,
  as an SR shaper lays them, and keeps its root without launch time: `off` lays the root (a host
  without launch time calls it before the socket opens) and removes only the etf. The order
  assumes the lowest queue has the highest priority, as on igb and igc (I210 Datasheet Section
  7.2.7.4), the drivers with AF_XDP launch time; an mqprio root of another handle goes first.
