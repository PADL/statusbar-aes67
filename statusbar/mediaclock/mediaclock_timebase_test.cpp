// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/mediaclock/mediaclock_timebase.hpp"

#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

using namespace statusbar::mediaclock;

namespace {

// 2026-10-22 in TAI nanoseconds: the fit must stay exact at today's epoch.
constexpr std::int64_t NOW_NS = 1'792'627'237'000'000'000LL;
constexpr std::int64_t MS = 1'000'000;
constexpr std::int64_t PASS_NS = 10 * MS;  // one reactor pass
constexpr double SAMPLE_NS = 1e9 / 48000.0;

/// A source on its own oscillator: frame n at PTP time base + n / (fs * (1 + r)), plus jitter.
class Source
{
  public:
    Source(std::uint32_t rate_hz, std::uint64_t frames_per_pair, double rate_offset, std::int64_t jitter_ns) noexcept
        : rate_hz_{rate_hz}
        , frames_per_pair_{frames_per_pair}
        , rate_offset_{rate_offset}
        , jitter_ns_{jitter_ns}
    {
        roll();
    }

    /// Source time at PTP time t, ns since the epoch began: the truth the fit estimates.
    [[nodiscard]] auto source_ns_at(std::int64_t t) const noexcept -> double
    {
        return (static_cast<double>(base_count_) * 1e9 / rate_hz_) + (static_cast<double>(t - base_ptp_) * (1.0 + rate_offset_));
    }

    auto feed_until(TimebaseEstimator& fit, std::int64_t now) noexcept -> void
    {
        while (next_reference() <= now) {
            fit.feed(ClockSample{static_cast<std::uint64_t>(next_reference()), next_, rate_hz_, epoch_});
            next_ += frames_per_pair_;
            outliers_ -= outliers_ > 0 ? 1 : 0;
            roll();
        }
    }

    /// Source time at PTP time t in integer ns, for a 1 GHz source started at `count`: exact at
    /// today's epoch, where a double is not.
    [[nodiscard]] auto absolute_ns_at(std::int64_t t) const noexcept -> std::int64_t
    {
        std::int64_t const dt = t - base_ptp_;
        return static_cast<std::int64_t>(base_count_) + dt + std::llround(rate_offset_ * static_cast<double>(dt));
    }

    /// Start the count at `count` (a 1 GHz source's own time, ns) instead of zero.
    auto start_at(std::uint64_t count) noexcept -> void
    {
        rebase();
        base_count_ = count;
        next_ = count;
    }

    /// Skip pairs up to PTP time t: the source falls silent.
    auto skip_until(std::int64_t now) noexcept -> void
    {
        while (next_reference() <= now) {
            next_ += frames_per_pair_;
        }
    }

    auto set_rate_offset(double r) noexcept -> void
    {
        rebase();
        rate_offset_ = r;
    }

    /// Frames from now on arrive `delta_ns` of PTP time early: a phase step of the source.
    auto step(std::int64_t delta_ns) noexcept -> void
    {
        rebase();
        base_ptp_ -= delta_ns;
    }

    /// The count restarts from zero in a new epoch, at a new rate and rate offset.
    auto restart(std::uint32_t rate_hz, std::uint64_t frames_per_pair, double r) noexcept -> void
    {
        rebase();
        base_count_ = 0;
        next_ = 0;
        rate_hz_ = rate_hz;
        frames_per_pair_ = frames_per_pair;
        rate_offset_ = r;
        ++epoch_;
    }

    auto outliers(int n, std::int64_t extra_ns) noexcept -> void
    {
        outliers_ = n;
        outlier_ns_ = extra_ns;
    }

  private:
    [[nodiscard]] auto ptp_of(std::uint64_t count) const noexcept -> std::int64_t
    {
        double const s = static_cast<double>(count - base_count_) * 1e9 / rate_hz_;
        return base_ptp_ + std::llround(s / (1.0 + rate_offset_));
    }

    [[nodiscard]] auto next_reference() const noexcept -> std::int64_t
    {
        return ptp_of(next_) + jitter_ + (outliers_ > 0 ? outlier_ns_ : 0);
    }

    auto rebase() noexcept -> void
    {
        base_ptp_ = ptp_of(next_);
        base_count_ = next_;
    }

    auto roll() noexcept -> void
    {
        rng_ ^= rng_ << 13U;
        rng_ ^= rng_ >> 17U;
        rng_ ^= rng_ << 5U;
        jitter_ =
            jitter_ns_ == 0 ? 0 : static_cast<std::int64_t>(rng_ % static_cast<std::uint32_t>((2 * jitter_ns_) + 1)) - jitter_ns_;
    }

    std::uint32_t rate_hz_;
    std::uint64_t frames_per_pair_;
    double rate_offset_;
    std::int64_t jitter_ns_;
    std::int64_t base_ptp_{NOW_NS};
    std::uint64_t base_count_{0};
    std::uint64_t next_{0};
    std::uint32_t epoch_{0};
    std::uint32_t rng_{0x12345678U};
    std::int64_t jitter_{0};
    int outliers_{0};
    std::int64_t outlier_ns_{0};
};

/// What the tick sees across passes: the largest change a publish makes to domain time at its
/// instant, and the largest departure of a pass's domain-time advance from its PTP advance.
struct Continuity
{
    std::int64_t prev_now{0};
    std::int64_t prev_domain{0};
    std::int64_t max_publish_jump{0};
    std::int64_t max_advance_error{0};

    auto observe(Timebase const& before, Timebase const& after, std::int64_t now) noexcept -> void
    {
        std::int64_t const domain = after.to_domain(now);
        max_publish_jump = std::max(max_publish_jump, std::abs(domain - before.to_domain(now)));
        if (prev_now != 0) {
            max_advance_error = std::max(max_advance_error, std::abs((domain - prev_domain) - (now - prev_now)));
        }
        prev_now = now;
        prev_domain = domain;
    }
};

/// Run reactor passes for `duration_ns` of PTP time, calling on_pass(before, changed) after each.
template <class F>
auto run_for(TimebaseEstimator& fit, Source& source, std::int64_t& now, std::int64_t duration_ns, F const& on_pass) -> void
{
    std::int64_t const end = now + duration_ns;
    while (now < end) {
        now += PASS_NS;
        source.feed_until(fit, now);
        Timebase const before = fit.timebase();
        bool const changed = fit.update(now);
        on_pass(before, changed);
    }
}

auto run_for(TimebaseEstimator& fit, Source& source, std::int64_t& now, std::int64_t duration_ns) -> void
{
    run_for(fit, source, now, duration_ns, [](Timebase const&, bool) {});
}

/// PTP time of the first pass in which the fit reports locked, run from now; 0 if none.
auto time_to_lock(TimebaseEstimator& fit, Source& source, std::int64_t& now, std::int64_t limit_ns) -> std::int64_t
{
    std::int64_t const start = now;
    std::int64_t locked_at = 0;
    run_for(fit, source, now, limit_ns, [&](Timebase const&, bool) {
        if (locked_at == 0 && fit.status().locked) {
            locked_at = now - start;
        }
    });
    return locked_at;
}

}  // namespace

TEST(mediaclock_timebase, timebase_round_trips_within_a_nanosecond_over_a_day)
{
    constexpr std::int64_t DAY_NS = 86'400LL * 1'000'000'000LL;
    for (double const r : {100e-6, -250e-6, 0.0}) {
        Timebase const tb{.reference_ns = NOW_NS, .domain_ns = NOW_NS + 12'345, .rate_offset = r, .epoch = 1, .running = true};
        EXPECT_EQ(tb.to_domain(NOW_NS), NOW_NS + 12'345);
        for (std::int64_t dt = -DAY_NS; dt <= DAY_NS; dt += (DAY_NS / 1000) + 7) {
            std::int64_t const t = NOW_NS + dt;
            std::int64_t const d = tb.to_domain(t);
            auto const exact = static_cast<long double>(NOW_NS + 12'345 + dt) + (static_cast<long double>(r) * dt);
            EXPECT_TRUE(std::fabs(static_cast<long double>(d) - exact) <= 0.5L);
            EXPECT_TRUE(std::abs(tb.from_domain(d) - t) <= 1);
        }
    }
    Timebase const id = Timebase::identity();
    EXPECT_EQ(id.to_domain(NOW_NS), NOW_NS);
    EXPECT_EQ(id.from_domain(NOW_NS), NOW_NS);
    EXPECT_FALSE(id.running);
}

TEST(mediaclock_timebase, a_100_ppm_source_locks_within_its_window)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::avb_stream()};
    Source source{48000, 48, 100e-6, 1'000};
    std::int64_t now = NOW_NS;
    EXPECT_FALSE(fit.timebase().running);
    std::int64_t const locked_at = time_to_lock(fit, source, now, 3'000 * MS);
    // 256 pairs spanning 250 ms, seen at the next 10 ms pass.
    EXPECT_TRUE(locked_at >= 250 * MS);
    EXPECT_TRUE(locked_at <= 270 * MS);
    auto const status = fit.status();
    EXPECT_TRUE(status.locked);
    EXPECT_TRUE(status.reference_present);
    EXPECT_TRUE(std::fabs(status.rate_offset_ppm - 100.0) < 0.1);
    EXPECT_TRUE(status.residual_ns < 1'000);
    EXPECT_EQ(status.pairs, std::uint32_t{2048});
    EXPECT_EQ(status.steps, std::uint32_t{0});
    EXPECT_TRUE(fit.timebase().running);
    EXPECT_EQ(fit.timebase().epoch, std::uint32_t{1});
}

TEST(mediaclock_timebase, a_locked_fit_predicts_the_index_within_a_tenth_of_a_sample)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::avb_stream()};
    Source source{48000, 48, 100e-6, 1'000};
    std::int64_t now = NOW_NS;
    EXPECT_TRUE(time_to_lock(fit, source, now, 1'000 * MS) != 0);
    auto const clock = PtpMediaClock::make(48000).value();
    // Domain time is source time plus the epoch's constant: the error is constant to the tolerance.
    std::int64_t lo = INT64_MAX;
    std::int64_t hi = INT64_MIN;
    bool indices_agree = true;
    run_for(fit, source, now, 5'000 * MS, [&](Timebase const&, bool) {
        std::int64_t const ahead = now + PASS_NS;  // used until the next pass publishes
        std::int64_t const error = fit.timebase().to_domain(ahead) - std::llround(source.source_ns_at(ahead));
        lo = std::min(lo, error);
        hi = std::max(hi, error);
        std::int64_t const expected = std::llround(source.source_ns_at(ahead)) + lo;
        std::uint64_t const got = clock.index_at(static_cast<std::uint64_t>(fit.timebase().to_domain(ahead)));
        std::uint64_t const want = clock.index_at(static_cast<std::uint64_t>(expected));
        indices_agree = indices_agree && (got - want + 1) <= 2;  // floor boundaries only
    });
    EXPECT_TRUE(fit.status().locked);
    EXPECT_TRUE(static_cast<double>(hi - lo) < SAMPLE_NS / 10);
    EXPECT_TRUE(indices_agree);
}

TEST(mediaclock_timebase, a_phase_step_resets_and_relocks_without_a_jump_in_domain_time)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::avb_stream()};
    Source source{48000, 48, 100e-6, 1'000};
    std::int64_t now = NOW_NS;
    Continuity continuity;
    run_for(fit, source, now, 3'000 * MS, [&](Timebase const& before, bool) { continuity.observe(before, fit.timebase(), now); });
    EXPECT_TRUE(fit.status().locked);

    // The talker flips from its own oscillator to gPTP: a 500 us phase step and r -> 0.
    std::int64_t const step_at = now;
    source.step(500'000);
    source.set_rate_offset(0.0);
    bool saw_holdover = false;
    std::int64_t relocked_at = 0;
    run_for(fit, source, now, 2'000 * MS, [&](Timebase const& before, bool) {
        continuity.observe(before, fit.timebase(), now);
        auto const status = fit.status();
        saw_holdover = saw_holdover || status.holdover;
        if (relocked_at == 0 && saw_holdover && status.locked) {
            relocked_at = now - step_at;
        }
        EXPECT_TRUE(fit.timebase().running);
    });
    auto const status = fit.status();
    EXPECT_EQ(status.steps, std::uint32_t{1});
    EXPECT_TRUE(status.last_step_ns > step_at && status.last_step_ns <= step_at + PASS_NS);
    EXPECT_TRUE(saw_holdover);
    EXPECT_TRUE(relocked_at > 250 * MS && relocked_at <= 280 * MS);
    EXPECT_TRUE(status.locked);
    EXPECT_TRUE(std::fabs(status.rate_offset_ppm) < 0.1);
    EXPECT_EQ(fit.timebase().epoch, std::uint32_t{2});
    // Neither a publish nor a pass moves domain time by more than the slope accounts for.
    EXPECT_TRUE(continuity.max_publish_jump < 250);
    EXPECT_TRUE(continuity.max_advance_error < 1'100);
}

TEST(mediaclock_timebase, a_single_outlier_is_dropped_not_a_step)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::avb_stream()};
    Source source{48000, 48, 100e-6, 1'000};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 1'000 * MS);
    EXPECT_TRUE(fit.status().locked);
    double const ppm = fit.status().rate_offset_ppm;

    source.outliers(1, 100'000);
    run_for(fit, source, now, 100 * MS, [&](Timebase const&, bool) { EXPECT_TRUE(fit.status().locked); });
    EXPECT_EQ(fit.status().steps, std::uint32_t{0});
    EXPECT_EQ(fit.status().outliers, std::uint32_t{1});

    // Two in a row are still outliers under a three-pair step.
    source.outliers(2, -100'000);
    run_for(fit, source, now, 100 * MS, [&](Timebase const&, bool) { EXPECT_TRUE(fit.status().locked); });
    EXPECT_EQ(fit.status().steps, std::uint32_t{0});
    EXPECT_EQ(fit.status().outliers, std::uint32_t{3});
    EXPECT_TRUE(std::fabs(fit.status().rate_offset_ppm - ppm) < 0.1);
    EXPECT_TRUE(fit.status().residual_ns < 1'000);
}

TEST(mediaclock_timebase, an_outlier_while_the_window_fills_does_not_delay_the_lock)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::avb_stream()};
    Source source{48000, 48, 100e-6, 1'000};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 100 * MS);
    source.outliers(1, 100'000);
    std::int64_t const locked_at = time_to_lock(fit, source, now, 3'000 * MS);
    EXPECT_TRUE(locked_at >= 150 * MS && locked_at <= 170 * MS);
    EXPECT_EQ(fit.status().outliers, std::uint32_t{1});
    EXPECT_EQ(fit.status().steps, std::uint32_t{0});
}

TEST(mediaclock_timebase, sparse_pairs_lock_after_four_reports)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::rtcp_sender_reports()};
    Source source{48000, 48000, 100e-6, 1'000};  // one sender report a second
    std::int64_t now = NOW_NS;
    std::int64_t const locked_at = time_to_lock(fit, source, now, 10'000 * MS);
    // Reports at 0, 1, 2 and 3 s (less 100 ppm): locked by the pass after the fourth.
    EXPECT_TRUE(locked_at > 2'990 * MS && locked_at <= 3'000 * MS);
    run_for(fit, source, now, 30'000 * MS);
    auto const status = fit.status();
    EXPECT_TRUE(status.locked);
    EXPECT_EQ(status.pairs, std::uint32_t{16});
    EXPECT_TRUE(std::fabs(status.rate_offset_ppm - 100.0) < 0.1);
}

TEST(mediaclock_timebase, crf_pairs_lock_in_a_quarter_second)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf()};
    Source source{48000, 96, -40e-6, 1'000};  // 500 timestamps a second
    std::int64_t now = NOW_NS;
    std::int64_t const locked_at = time_to_lock(fit, source, now, 3'000 * MS);
    // 128 pairs spanning 254 ms, seen at the next 10 ms pass.
    EXPECT_TRUE(locked_at >= 250 * MS && locked_at <= 260 * MS);
    run_for(fit, source, now, 5'000 * MS);
    auto const status = fit.status();
    EXPECT_TRUE(status.locked);
    EXPECT_EQ(status.pairs, std::uint32_t{1024});
    EXPECT_TRUE(std::fabs(status.rate_offset_ppm + 40.0) < 0.1);
}

TEST(mediaclock_timebase, phc_pairs_lock_within_a_second)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::phc_pairs()};
    Source source{1'000'000'000, 62'500'000, 25e-6, 200};  // 16 cross-timestamps a second
    std::int64_t now = NOW_NS;
    std::int64_t const locked_at = time_to_lock(fit, source, now, 5'000 * MS);
    // 16 pairs spanning 937.5 ms, seen at the next 10 ms pass.
    EXPECT_TRUE(locked_at >= 930 * MS && locked_at <= 950 * MS);
    run_for(fit, source, now, 10'000 * MS);
    auto const status = fit.status();
    EXPECT_TRUE(status.locked);
    EXPECT_EQ(status.pairs, std::uint32_t{64});
    EXPECT_TRUE(std::fabs(status.rate_offset_ppm - 25.0) < 0.1);
}

TEST(mediaclock_timebase, an_absolute_fit_reports_the_source_time_itself)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::phc_pairs()};
    Source source{1'000'000'000, 62'500'000, -37e-6, 100};
    source.start_at(static_cast<std::uint64_t>(NOW_NS + 37'000'000'123LL));  // another grandmaster's time
    std::int64_t now = NOW_NS;
    EXPECT_TRUE(time_to_lock(fit, source, now, 5'000 * MS) != 0);
    EXPECT_EQ(fit.timebase().epoch, std::uint32_t{1});
    std::int64_t worst = 0;
    run_for(fit, source, now, 30'000 * MS, [&](Timebase const&, bool) {
        std::int64_t const ahead = now + PASS_NS;
        worst = std::max(worst, std::abs(fit.timebase().to_domain(ahead) - source.absolute_ns_at(ahead)));
    });
    EXPECT_TRUE(fit.status().locked);
    EXPECT_TRUE(std::fabs(fit.status().rate_offset_ppm + 37.0) < 0.1);
    EXPECT_TRUE(worst < 50);
}

TEST(mediaclock_timebase, an_absolute_fit_jumps_with_its_source)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::phc_pairs()};
    Source source{1'000'000'000, 62'500'000, 10e-6, 100};
    source.start_at(static_cast<std::uint64_t>(NOW_NS - 5'000'000'000LL));
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 10'000 * MS);
    EXPECT_TRUE(fit.status().locked);

    // A new grandmaster 37 s ahead, at another rate: the domain stops, then re-anchors on it.
    constexpr std::int64_t JUMP_NS = 37'000'000'000LL;
    std::int64_t const step_at = now;
    source.step(JUMP_NS);
    source.set_rate_offset(-20e-6);
    std::int64_t stopped_at = 0;
    std::int64_t relocked_at = 0;
    std::int64_t max_publish_jump = 0;
    run_for(fit, source, now, 5'000 * MS, [&](Timebase const& before, bool changed) {
        Timebase const after = fit.timebase();
        max_publish_jump = std::max(max_publish_jump, std::abs(after.to_domain(now) - before.to_domain(now)));
        if (stopped_at == 0 && !after.running) {
            stopped_at = now - step_at;
            EXPECT_TRUE(changed);
            EXPECT_FALSE(fit.status().holdover);
        }
        if (relocked_at == 0 && stopped_at != 0 && after.running) {
            relocked_at = now - step_at;
        }
    });
    auto const status = fit.status();
    // Stopped at the third held pair; the window restarts from them and locks 16 pairs on.
    EXPECT_TRUE(stopped_at > 0 && stopped_at <= 200 * MS);
    EXPECT_TRUE(relocked_at > 930 * MS && relocked_at <= 1'010 * MS);
    EXPECT_TRUE(status.locked);
    EXPECT_EQ(status.steps, std::uint32_t{1});
    EXPECT_EQ(fit.timebase().epoch, std::uint32_t{2});
    EXPECT_TRUE(std::fabs(status.rate_offset_ppm + 20.0) < 0.1);
    EXPECT_TRUE(max_publish_jump > JUMP_NS - MS && max_publish_jump < JUMP_NS + MS);
    EXPECT_TRUE(std::abs(fit.timebase().to_domain(now + PASS_NS) - source.absolute_ns_at(now + PASS_NS)) < 50);
}

TEST(mediaclock_timebase, a_new_epoch_keeps_domain_time_continuous)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::avb_stream()};
    Source source{48000, 48, 100e-6, 1'000};
    std::int64_t now = NOW_NS;
    Continuity continuity;
    run_for(fit, source, now, 3'000 * MS, [&](Timebase const& before, bool) { continuity.observe(before, fit.timebase(), now); });
    EXPECT_TRUE(fit.status().locked);

    // The source restarts at 96 kHz on another oscillator: count from zero, a new epoch.
    source.restart(96000, 96, -50e-6);
    bool saw_holdover = false;
    run_for(fit, source, now, 1'000 * MS, [&](Timebase const& before, bool) {
        continuity.observe(before, fit.timebase(), now);
        saw_holdover = saw_holdover || fit.status().holdover;
    });
    auto const status = fit.status();
    EXPECT_TRUE(saw_holdover);
    EXPECT_TRUE(status.locked);
    EXPECT_EQ(status.steps, std::uint32_t{0});
    EXPECT_TRUE(std::fabs(status.rate_offset_ppm + 50.0) < 0.2);
    EXPECT_EQ(fit.timebase().epoch, std::uint32_t{2});
    EXPECT_TRUE(continuity.max_publish_jump < 250);
    EXPECT_TRUE(continuity.max_advance_error < 1'100);
}

TEST(mediaclock_timebase, a_silent_source_holds_over_then_unlocks)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::avb_stream()};
    Source source{48000, 48, 100e-6, 1'000};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 1'000 * MS);
    EXPECT_TRUE(fit.status().locked);
    Timebase const last = fit.timebase();
    std::int64_t const silent_at = now;

    // No pairs: holdover once the reference times out, on the last line, for 5 s.
    std::int64_t holdover_at = 0;
    std::int64_t unlocked_at = 0;
    while (now < silent_at + (6'000 * MS)) {
        now += PASS_NS;
        source.skip_until(now);
        bool const changed = fit.update(now);
        auto const status = fit.status();
        if (holdover_at == 0 && status.holdover) {
            holdover_at = now - silent_at;
        }
        if (unlocked_at == 0 && !status.locked && !status.holdover) {
            unlocked_at = now - silent_at;
            EXPECT_TRUE(changed);
            EXPECT_FALSE(fit.timebase().running);
        }
        if (unlocked_at == 0) {
            EXPECT_FALSE(changed);
            EXPECT_TRUE(fit.timebase() == last);
        }
    }
    EXPECT_TRUE(holdover_at >= 100 * MS && holdover_at <= 110 * MS);
    EXPECT_TRUE(unlocked_at > 5'000 * MS && unlocked_at <= 5'010 * MS);
    EXPECT_FALSE(fit.status().reference_present);

    // The source returns: it relocks on the line it left, without a jump.
    Continuity continuity;
    continuity.observe(fit.timebase(), fit.timebase(), now);
    run_for(fit, source, now, 1'000 * MS, [&](Timebase const& before, bool) { continuity.observe(before, fit.timebase(), now); });
    EXPECT_TRUE(fit.status().locked);
    EXPECT_TRUE(fit.timebase().running);
    EXPECT_TRUE(continuity.max_publish_jump < 250);
}

TEST_MAIN(statusbar_mediaclock, mediaclock_timebase_test)
