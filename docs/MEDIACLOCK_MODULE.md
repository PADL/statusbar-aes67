[← back to module index](README.md)

# mediaclock

`PtpMediaClock`: the PTP-locked media clock of RFC 7273 `a=mediaclk:direct`, mapping a
sample index to PTP time and back, and to the 32-bit RTP and AVTP timestamps on the wire.
`Timebase` and `TimebaseFit`: a media clock that is not PTP-locked, as an affine map from
PTP time fitted from its source's (PTP time, sample count) pairs.

## Overview

AES67 streams use the PTP clock itself as the media clock (RFC 7273 §5.2): sample index
`n` is presented at PTP time `n / fs`. A sample's index is therefore a pure function of
PTP time, and every device on the same grandmaster agrees on it without exchanging
anything. An RTP timestamp is the low 32 bits of the index plus the stream's direct
offset; an AVTP presentation time is `time_of(index)` modulo 2^32 ns. A bridge maps a
sample from one wire to the other with this arithmetic alone: no rate estimation, no
anchoring, no state.

`PtpMediaClock` is header-only and `constexpr` throughout.

A source on its own oscillator (an AVB talker on internal clock, an RTP sender asserting
`mediaclk:sender`, a CRF stream) defines a clock domain whose time is
`t_d = domain_ns + (t - reference_ns) * (1 + r)` for PTP time `t`: a `Timebase`. Its index
at rate `fs` is `PtpMediaClock{fs}.index_at(t_d)`, so every rate in a domain shares one
timebase. `TimebaseFit` estimates the map by least squares over a window of pairs the
source supplies (AVTP presentation time and frame count, an RTCP sender report's NTP/RTP
pair converted to PTP time, a CRF timestamp), detects steps and source restarts, and keeps
domain time continuous across them: each lock joins the new line to the one in use at that
instant, so only the slope changes.

## Key types

- `PtpMediaClock` — the clock for one sample rate; `make(fs)` returns `nullopt` for a rate it does not support.
  - `index_at(ptp_ns)` — the latest sample presented at or before a PTP time.
  - `time_of(index)` — the PTP time a sample is presented, rounded up to the nanosecond; `index_at(time_of(n)) == n`.
  - `unwrap(timestamp, reference)` — the full index of a 32-bit wire timestamp nearest a reference index.
  - `rtp_timestamp(index, offset)` / `index_of_rtp(timestamp, offset, reference)` — RTP timestamps under `mediaclk:direct=<offset>`.
  - `avtp_timestamp(index)` — a sample's AVTP presentation time, mod 2^32 ns.
  - `samples_per_tick()` — samples per 125 µs (AVB class A) tick: 6, 12 or 24.
- `Timebase` — 32 bytes, trivially copyable: `reference_ns`, `domain_ns`, `rate_offset`, `epoch`, `running`.
  - `to_domain(t_phc)` / `from_domain(t_d)` — exact within a nanosecond over a day either side of the anchor.
  - `identity()` — domain time is PTP time, not running: the map before a first lock.
- `ClockSample` — one pair: `reference_ns` (PTP), `count` (frames since the epoch), `rate_hz`, `epoch`.
- `TimebaseFitParams` — a preset per source kind; `avb_stream()`, `rtcp_sender_reports()`, `crf()`.
- `TimebaseFit` — `feed(sample)` per pair, `update(now_ns)` per pass (true: publish `timebase()`), `status()`.
- `TimebaseFitStatus` — `locked`, `holdover`, `reference_present`, `rate_offset_ppm`, `residual_ns`, `last_step_ns`, `steps`, `outliers`, `pairs`.

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

A foreign domain: the fit on the control thread, the tick reading the published map.

```cpp
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/mediaclock/mediaclock_timebase.hpp"

#include <span>

using namespace statusbar::mediaclock;

statusbar::itc::SeqlockValue<Timebase> published{Timebase::identity()};
TimebaseFit fit{TimebaseFitParams::avb_stream()};

auto on_pass(std::span<ClockSample const> pairs, std::int64_t ptp_now_ns) -> void  // control thread
{
    for (auto const& pair : pairs) {
        fit.feed(pair);
    }
    if (fit.update(ptp_now_ns)) {
        published.store(fit.timebase());
    }
}

auto domain_index(std::int64_t ptp_now_ns, PtpMediaClock clock) -> std::uint64_t  // tick
{
    Timebase const tb = published.load();
    return clock.index_at(static_cast<std::uint64_t>(tb.to_domain(ptp_now_ns)));
}
```

## Headers

- `statusbar/mediaclock/mediaclock_ptp_clock.hpp` — `PtpMediaClock`.
- `statusbar/mediaclock/mediaclock_timebase.hpp` — `Timebase`, `ClockSample`, `TimebaseFitParams`, `TimebaseFit`.

## Dependencies

- **Statusbar modules:** none.
- **System / external:** `<cstdint>`, `<optional>`, `<array>`, `<cmath>`.
- Publishing a `Timebase` across threads uses `core`'s `itc::SeqlockValue`, which the module does not itself depend on.

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
- `TimebaseFit` runs on a control thread: `update()` refits the whole window (2048 pairs,
  a few microseconds) once per pass in which pairs arrived. It never allocates; the
  window is a fixed array of `MAX_WINDOW_PAIRS`, so the object is about 33 KB.
- The reader side is one seqlock load and one `to_domain()` per tick; a load retries only
  across a publish, at most one per reactor pass.
- Presets (`TimebaseFitParams`), starting values to be tuned against real sources:

  | Preset | Window | Lock after | Step | Reference timeout |
  |---|---|---|---|---|
  | `avb_stream()` | 2048 pairs | 256 pairs spanning 250 ms | 3 residuals over 20 µs | 100 ms |
  | `rtcp_sender_reports()` | 16 | 4 | 2 over 20 µs | 3 s |
  | `crf()` | 128 | 32 | 3 over 20 µs | 100 ms |

  All lock while the RMS residual is under 2 µs and |r| under 500 ppm, and hold over for
  5 s after the last good fit.
- A residual over the step threshold is held; fewer than `step_pairs` in a row are dropped
  as outliers, `step_pairs` in a row are a step and restart the window from them. A new
  `epoch` or `rate_hz` restarts it without counting a step. Either way the domain runs on
  its last line (holdover) until the new window locks.
- Residuals are checked once a fit spans `lock_pairs / 8` pairs (at least 3); an outlier
  among the very first pairs makes the next ones look like a step, which restarts the
  window without being counted (only a fit of `lock_pairs` or more counts steps).
- No pair for `reference_timeout_ns` resets the window: the line after a silence is
  fitted from fresh pairs, under the same lock criteria.
- Before the first lock the map is the identity and the first lock joins it, so a fresh
  domain's index starts near PTP's. When holdover expires `running` goes false but the
  line is kept, so a later lock still joins it without a jump.
- A pair whose PTP time is not after the newest one is ignored: a step backwards by more
  than the pair interval loses the pairs until they pass it.

## Further reading

- RFC 7273 §5.2 — the `direct` media clock.
- AES67-2018 — media clock and RTP timestamp requirements.
- RFC 7273 §5 — `mediaclk:sender`, a media clock the sender asserts.
- RFC 3550 §6.4.1 — sender reports pairing an RTP timestamp with a wall-clock time.
- Milan v1.2 §7.3 — the CRF stream.
- [`aes67`](AES67_MODULE.md) — receivers and senders that address frames by this index.
