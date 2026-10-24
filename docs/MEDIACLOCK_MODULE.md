[← back to module index](README.md)

# mediaclock

`PtpMediaClock`: the PTP-locked media clock of RFC 7273 `a=mediaclk:direct`, mapping a
sample index to PTP time and back, and to the 32-bit RTP and AVTP timestamps on the wire.

## Overview

AES67 streams use the PTP clock itself as the media clock (RFC 7273 §5.2): sample index
`n` is presented at PTP time `n / fs`. A sample's index is therefore a pure function of
PTP time, and every device on the same grandmaster agrees on it without exchanging
anything. An RTP timestamp is the low 32 bits of the index plus the stream's direct
offset; an AVTP presentation time is `time_of(index)` modulo 2^32 ns. A bridge maps a
sample from one wire to the other with this arithmetic alone: no rate estimation, no
anchoring, no state.

The module is header-only and `constexpr` throughout.

## Key types

- `PtpMediaClock` — the clock for one sample rate; `make(fs)` returns `nullopt` for a rate it does not support.
  - `index_at(ptp_ns)` — the latest sample presented at or before a PTP time.
  - `time_of(index)` — the PTP time a sample is presented, rounded up to the nanosecond; `index_at(time_of(n)) == n`.
  - `unwrap(timestamp, reference)` — the full index of a 32-bit wire timestamp nearest a reference index.
  - `rtp_timestamp(index, offset)` / `index_of_rtp(timestamp, offset, reference)` — RTP timestamps under `mediaclk:direct=<offset>`.
  - `avtp_timestamp(index)` — a sample's AVTP presentation time, mod 2^32 ns.
  - `samples_per_tick()` — samples per 125 µs (AVB class A) tick: 6, 12 or 24.

## Quick example

```cpp
#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"

using statusbar::mediaclock::PtpMediaClock;

auto place(std::uint64_t ptp_now_ns, std::uint32_t rtp_ts, std::uint32_t offset) -> std::uint64_t
{
    auto const clock = PtpMediaClock::make(48000);
    if (!clock) {
        return 0;
    }
    std::uint64_t const now = clock->index_at(ptp_now_ns);
    return PtpMediaClock::index_of_rtp(rtp_ts, offset, now);  // the packet's first sample
}
```

## Headers

- `statusbar/mediaclock/mediaclock_ptp_clock.hpp` — `PtpMediaClock`.

## Dependencies

- **Statusbar modules:** none.
- **System / external:** `<cstdint>`, `<optional>`.

## Notes & caveats

- The arithmetic is exact and uses no 128-bit types: a time is split into whole seconds
  and nanoseconds and only the sub-second part is scaled, so the largest intermediate is
  `1e9 * fs` (below 2^48 at 192 kHz).
- Only the 48 kHz family is supported (`fs` a multiple of 24 kHz), which keeps every
  125 µs tick a whole number of samples. Check a rate taken from an SDP with
  `supports()` or `make()`; do not assume it.
- `unwrap()` is unambiguous within ±2^31 samples of the reference, about 12 hours at
  48 kHz.
- Subtract the stream's direct offset before unwrapping (`index_of_rtp()` does).

## Further reading

- RFC 7273 §5.2 — the `direct` media clock.
- AES67-2018 — media clock and RTP timestamp requirements.
- [`aes67`](AES67_MODULE.md) — receivers and senders that address frames by this index.
