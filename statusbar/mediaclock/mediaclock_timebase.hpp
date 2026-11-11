#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// A clock domain's timebase: an affine map from PTP time to the domain's time, fitted by least
/// squares from (PTP time, sample count) pairs that the domain's source supplies. The fit runs on
/// a control thread; the Timebase it publishes is read on the real-time tick.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace statusbar::mediaclock {

/// The map t_d = domain_ns + (t - reference_ns) * (1 + rate_offset), anchored at PTP time
/// reference_ns. Trivially copyable, for publication through an itc::SeqlockValue.
struct Timebase
{
    std::int64_t reference_ns{0};  ///< PTP time of the anchor, ns
    std::int64_t domain_ns{0};     ///< domain time at the anchor, ns
    double rate_offset{0.0};       ///< r: domain ns per PTP ns, minus one (1e-6 is 1 ppm)
    /// Bumped at each lock (a join, or an absolute fit's re-anchor, which may jump) and at a free_run() of a
    /// follow_phase fit that had locked.
    std::uint32_t epoch{0};
    bool running{false};        ///< outputs in the domain send: locked or in holdover, a follow_phase fit's always
    bool locked{false};         ///< the fit is locked to its source (not before a lock, not in holdover)
    std::uint16_t restarts{0};  ///< a follow_phase fit's media clock restarts: a talker toggles mr on each change

    /// The provisional map before a first lock: domain time is PTP time; not running.
    [[nodiscard]] static constexpr auto identity() noexcept -> Timebase { return Timebase{}; }

    /// Domain time at PTP time t_phc, ns: one subtraction, one multiply-add, one rounding.
    [[nodiscard]] auto to_domain(std::int64_t t_phc) const noexcept -> std::int64_t
    {
        std::int64_t const dt = t_phc - reference_ns;
        return domain_ns + dt + std::llround(rate_offset * static_cast<double>(dt));
    }

    /// PTP time at domain time t_d, ns; inverts to_domain() within a nanosecond.
    [[nodiscard]] auto from_domain(std::int64_t t_d) const noexcept -> std::int64_t
    {
        std::int64_t const dd = t_d - domain_ns;
        return reference_ns + dd - std::llround(static_cast<double>(dd) * rate_offset / (1.0 + rate_offset));
    }

    /// Field-wise equality.
    friend constexpr auto operator==(Timebase const&, Timebase const&) noexcept -> bool = default;
};

static_assert(sizeof(Timebase) == 32);
static_assert(offsetof(Timebase, locked) == 29);
static_assert(offsetof(Timebase, restarts) == 30);
static_assert(std::is_trivially_copyable_v<Timebase>);

/// One pair from a domain's source: PTP time, frames since the source's epoch began, the
/// source's rate, and an epoch the source bumps when its count restarts.
struct ClockSample
{
    std::uint64_t reference_ns{0};  ///< PTP time of frame `count`, ns
    std::uint64_t count{0};         ///< frames since the epoch began
    std::uint32_t rate_hz{0};       ///< the source's nominal rate; 0 makes the pair ignored
    std::uint32_t epoch{0};         ///< changes when count restarts
};

static_assert(sizeof(ClockSample) == 24);

/// Starting values for one kind of source (window, lock, step, holdover); tuned on the bench.
struct TimebaseEstimatorParams
{
    std::uint32_t window_pairs{2048};  ///< pairs fitted, at most TimebaseEstimator::MAX_WINDOW_PAIRS
    /// A pair older than the newest by more than this leaves a window of over MIN_SPAN_PAIRS and lock_pairs, ns; 0: no limit.
    std::int64_t window_span_ns{0};
    std::uint32_t lock_pairs{256};                   ///< pairs needed before the fit may lock
    std::int64_t lock_span_ns{250'000'000};          ///< PTP time those pairs must span, ns
    std::int64_t lock_rms_ns{2'000};                 ///< locked while the RMS residual is under this, ns
    double lock_rate_ppm{500.0};                     ///< and |rate offset| is under this, ppm
    std::int64_t step_threshold_ns{20'000};          ///< a residual over this is held, not fitted, ns
    std::uint32_t step_pairs{3};                     ///< this many held in a row is a step
    std::int64_t step_span_ns{0};                    ///< and they span this much PTP time, ns: one rule at any cadence
    std::int64_t reference_timeout_ns{100'000'000};  ///< no pair for this long: reference absent, ns
    /// Nonzero: the timeout is this many of the window's mean pair intervals, within [reference_timeout_ns,
    /// reference_timeout_max_ns], and the ceiling while under two pairs are held: a sender's own cadence sets it.
    std::uint32_t timeout_intervals{0};
    std::int64_t reference_timeout_max_ns{0};  ///< the ceiling of a cadence-derived timeout, ns
    std::int64_t holdover_ns{5'000'000'000};   ///< run on the last line this long after the last good fit, ns
    bool absolute{false};                      ///< domain time is the source's own time: each lock re-anchors, not joins
    /// A media clock follower (clears `absolute`): runs from the start, holds over without end, each lock landing the
    /// source's frames on its frame grid; a loss of the reference, and a lock after it, after a source discontinuity or
    /// moving domain time by over RESTART_NS, each count a restart (IEEE 1722-2016 Clause 4.4.4.3).
    bool follow_phase{false};

    /// An AVB stream, one pair per millisecond (every 8th class A packet).
    [[nodiscard]] static constexpr auto avb_stream() noexcept -> TimebaseEstimatorParams { return TimebaseEstimatorParams{}; }

    /// RTCP sender reports (RFC 3550 Section 6.4.1), every one fed, from RFC 3550's randomised 5 s to IPMX's 10 ms (VSF
    /// TR-10-1 Section 8.10.1): 15 s of them (16 at least), locked on 4 spanning 2.5 s, lost after 3 mean intervals (1 to 20 s).
    [[nodiscard]] static constexpr auto rtcp_sender_reports() noexcept -> TimebaseEstimatorParams
    {
        return TimebaseEstimatorParams{
            .window_pairs = 2048,
            .window_span_ns = 15'000'000'000,
            .lock_pairs = 4,
            .lock_span_ns = 2'500'000'000,
            .step_pairs = 2,
            .step_span_ns = 2'000'000'000,
            .reference_timeout_ns = 1'000'000'000,
            .timeout_intervals = 3,
            .reference_timeout_max_ns = 20'000'000'000,
        };
    }

    /// A CRF stream (Milan v1.2 Section 7.3), 500 timestamps a second (48 kHz, timestamp_interval 96).
    [[nodiscard]] static constexpr auto crf() noexcept -> TimebaseEstimatorParams
    {
        return TimebaseEstimatorParams{.window_pairs = 1024, .lock_pairs = 128};
    }

    /// `params` as a media clock follower: follow_phase set (a fit then ignores `absolute`).
    [[nodiscard]] static constexpr auto follower(TimebaseEstimatorParams params) noexcept -> TimebaseEstimatorParams
    {
        params.follow_phase = true;
        return params;
    }

    /// A CRF stream a talker follows: follower(crf()).
    [[nodiscard]] static constexpr auto crf_follower() noexcept -> TimebaseEstimatorParams { return follower(crf()); }

    /// PHC-to-PHC cross-timestamps against a followed PTP instance, 16 a second; count is that
    /// PTP's time in ns at rate_hz 1e9, and domain time is that PTP's time (absolute).
    [[nodiscard]] static constexpr auto phc_pairs() noexcept -> TimebaseEstimatorParams
    {
        return TimebaseEstimatorParams{
            .window_pairs = 64,
            .lock_pairs = 16,
            .lock_span_ns = 900'000'000,
            .lock_rms_ns = 1'000,
            .step_threshold_ns = 2'000,
            .reference_timeout_ns = 500'000'000,
            .absolute = true,
        };
    }
};

/// A domain's fit state, for reporting.
struct TimebaseEstimatorStatus
{
    bool locked{false};             ///< the window meets the lock criteria and the reference is present
    bool holdover{false};           ///< running on the last line after losing lock
    bool reference_present{false};  ///< a pair arrived within reference_timeout_ns
    double rate_offset_ppm{0.0};    ///< the latest fit's rate offset, else the line's, ppm
    std::int64_t residual_ns{0};    ///< the latest fit's RMS residual, ns (0 with no fit)
    std::int64_t last_step_ns{0};   ///< PTP time of the last step's first pair, ns; 0 for none
    std::uint32_t steps{0};         ///< steps detected
    std::uint32_t outliers{0};      ///< held pairs dropped as outliers
    std::uint32_t pairs{0};         ///< pairs in the window
};

/// Least-squares fit of a domain's time against PTP time over a window of pairs; control thread only, no allocation.
/// feed() each pair, update() once per pass, and publish timebase() when it returns true. Each lock joins the new line
/// to the old, so domain time never jumps; an absolute fit re-anchors on the source's time and stops on a step.
class TimebaseEstimator
{
  public:
    /// The largest window_pairs a preset may ask for (the window is a fixed array).
    static constexpr std::size_t MAX_WINDOW_PAIRS = 2048;
    /// A follower's lock that moves domain time by more than this is a restart, whatever came before it, ns.
    static constexpr std::int64_t RESTART_NS = 1'000;
    /// The largest step_pairs a preset may ask for; a step spanning more pairs restarts the window from its last ones.
    static constexpr std::size_t MAX_STEP_PAIRS = 8;
    /// window_span_ns never trims a window to fewer pairs than this: a slow sender's fit keeps its averaging.
    static constexpr std::size_t MIN_SPAN_PAIRS = 16;

    /// A fit with the given preset: window_pairs clamped to [2, MAX_WINDOW_PAIRS], step_pairs to
    /// [1, MAX_STEP_PAIRS], lock_pairs to [2, window_pairs].
    explicit TimebaseEstimator(TimebaseEstimatorParams const& params) noexcept;

    /// Record one pair: a new epoch or rate resets the window; once a fit spans lock_pairs / 8 (at
    /// least 3), a residual over the step threshold is held and step_pairs in a row restart the
    /// window from them (a step, if the fit spanned lock_pairs).
    auto feed(ClockSample const& sample) noexcept -> void;

    /// Refit if pairs arrived, then move between locked, holdover and unlocked at PTP time now_ns;
    /// no pair for reference_timeout_ns resets the window. True when timebase() should be published.
    [[nodiscard]] auto update(std::int64_t now_ns) noexcept -> bool;

    /// The current line: identity() before the first lock, running while locked or in holdover (a follow_phase fit's
    /// always).
    [[nodiscard]] auto timebase() const noexcept -> Timebase { return published_; }

    /// A follow_phase fit (else nothing) stops following at now_ns: the line runs on at the reference's rate, continuous,
    /// in a new epoch, a restart once it had locked; the window dropped. Pairs fed afterwards are followed afresh.
    auto free_run(std::int64_t now_ns) noexcept -> void;

    /// A follow_phase fit (else nothing) leaves its source at now_ns as free_run() does (a restart once it had locked),
    /// then adopts another source kind's preset, clamped as constructed and kept a follower: the old source forgotten
    /// (window, reference, epoch), the line continuous.
    auto retune(std::int64_t now_ns, TimebaseEstimatorParams const& params) noexcept -> void;

    /// Lock state, rate offset, residual and step counts as of the last feed() and update().
    [[nodiscard]] auto status() const noexcept -> TimebaseEstimatorStatus;

    /// The preset in use, after clamping.
    [[nodiscard]] auto params() const noexcept -> TimebaseEstimatorParams const& { return params_; }

  private:
    struct Pair
    {
        std::int64_t reference_ns;
        std::int64_t source_ns;
    };

    enum class State : std::uint8_t
    {
        unlocked,
        locked,
        holdover,
    };

    auto push(Pair const& p) noexcept -> void;
    /// Drops the oldest pairs past window_span_ns of the newest, keeping MIN_SPAN_PAIRS and lock_pairs.
    auto trim_span() noexcept -> void;
    /// window_pairs, step_pairs and lock_pairs clamped to their ranges; absolute cleared on a follower.
    [[nodiscard]] static auto clamped(TimebaseEstimatorParams params) noexcept -> TimebaseEstimatorParams;
    auto reset_window() noexcept -> void;
    auto source_discontinuity() noexcept -> void;
    auto refit() noexcept -> void;
    /// Fits a least-squares line through the window, centred on `anchor`; false, the old line kept, when degenerate.
    [[nodiscard]] auto fit_line(Pair const& anchor) noexcept -> bool;
    /// The RMS residual of the window about the fitted line, ns.
    [[nodiscard]] auto rms_residual() const noexcept -> double;
    /// Restarts the window when the source's epoch or rate changes, and remembers them.
    auto track_source(ClockSample const& sample) noexcept -> void;
    /// Accepts a pair's reference time; false when out of order or repeated.
    [[nodiscard]] auto accept_reference(std::int64_t reference_ns) noexcept -> bool;
    /// Holds a pair off the fit when its residual says it may be a step; true when it was held.
    [[nodiscard]] auto screen_step(Pair const& p) noexcept -> bool;
    /// Holds a step candidate; enough in a row, spanning step_span_ns, restart the window from them (a step, once the
    /// fit was long).
    auto hold(Pair const& p) noexcept -> void;
    /// Restarts the window from the pairs held: a step when the fit spanned lock_pairs.
    auto confirm_step() noexcept -> void;
    /// The time without a pair after which the reference is absent: fixed, or from the window's cadence.
    [[nodiscard]] auto reference_timeout() const noexcept -> std::int64_t;
    /// Notes whether the reference is present at `now_ns`; pairs from before a silence are dropped.
    auto check_reference(std::int64_t now_ns) noexcept -> void;
    /// Publishes the locked line, joining it to the one in use when the lock is new; true when published.
    [[nodiscard]] auto publish_locked(std::int64_t now_ns, bool refitted) noexcept -> bool;
    /// Goes to holdover, then unlocked once the holdover runs out (a follow_phase fit never); true when the line stopped.
    [[nodiscard]] auto lose_lock(std::int64_t now_ns) noexcept -> bool;
    /// The domain constant joining the new line to the one in use at `now_ns`: on the source's frame grid when follow_phase.
    [[nodiscard]] auto join_constant(std::int64_t now_ns) const noexcept -> std::int64_t;
    /// A media clock restart: counted in the published line, which update() publishes.
    auto restart() noexcept -> void;
    /// Drops the window: the fit is unlocked until its source locks afresh.
    auto forget_source() noexcept -> void;
    /// free_run()'s new line: the one in use at `now_ns`, at the reference's rate, in a new epoch; a restart.
    auto leave_line(std::int64_t now_ns) noexcept -> void;
    /// A follower's lock at `now_ns`: a restart unless it regains the source it was following in place.
    auto notify_follow_lock(std::int64_t now_ns) noexcept -> void;
    [[nodiscard]] auto pair_at(std::size_t i) const noexcept -> Pair const&;
    [[nodiscard]] auto newest() const noexcept -> Pair const& { return pair_at(size_ - 1); }
    [[nodiscard]] auto residual(Pair const& p) const noexcept -> double;
    [[nodiscard]] auto fitted_source_ns(std::int64_t t_phc) const noexcept -> std::int64_t;
    [[nodiscard]] auto check_pairs() const noexcept -> std::size_t;
    template <class F>
    auto for_each_pair(F const& f) const noexcept -> void;
    [[nodiscard]] auto meets_lock() const noexcept -> bool;
    [[nodiscard]] auto line() const noexcept -> Timebase;

    TimebaseEstimatorParams params_;
    std::array<Pair, MAX_WINDOW_PAIRS> window_{};
    std::size_t head_{0};  // index of the oldest pair
    std::size_t size_{0};
    std::size_t pending_{0};                   // pairs since the last refit
    std::array<Pair, MAX_STEP_PAIRS> held_{};  // the newest of the pairs held, oldest first
    std::size_t held_count_{0};                // pairs held in a row (held_ keeps the last MAX_STEP_PAIRS)
    std::int64_t held_first_ns_{0};            // PTP time of the first pair held

    bool have_reference_{false};
    std::int64_t last_reference_ns_{0};  // newest pair accepted, fitted or held
    bool have_source_{false};
    std::uint32_t source_epoch_{0};
    std::uint32_t source_rate_hz_{0};

    // The latest fit: source_ns = anchor_source_ns_ + intercept_ + slope_ * (t - anchor_reference_ns_).
    bool fit_valid_{false};
    std::size_t fit_pairs_{0};
    std::int64_t anchor_reference_ns_{0};
    std::int64_t anchor_source_ns_{0};
    double intercept_{0.0};
    double slope_{1.0};
    double rms_ns_{0.0};

    State state_{State::unlocked};
    std::int64_t domain_constant_ns_{0};  // the epoch's constant: domain_ns = source_ns + this; 0 when absolute
    std::int64_t last_good_ns_{0};
    bool reference_present_{false};
    bool publish_pending_{false};   // the line changed outside a lock since the last update()
    bool following_{false};         // a follower locked to its source and has not lost it since
    bool source_restarted_{false};  // a step or new epoch of the source since the last lock
    Timebase published_{Timebase::identity()};
    std::int64_t last_step_ns_{0};
    std::uint32_t steps_{0};
    std::uint32_t outliers_{0};
};

}  // namespace statusbar::mediaclock
