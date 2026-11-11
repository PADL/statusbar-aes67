// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "padl/mediaclock/mediaclock_timebase.hpp"

#include "padl/mediaclock/mediaclock_ptp_clock.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace padl::mediaclock;

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

    /// Jitter of up to `ns` either way from now on.
    auto set_jitter(std::int64_t const ns) noexcept -> void { jitter_ns_ = ns; }

    /// PTP time of frame `count` of the current epoch, without jitter.
    [[nodiscard]] auto ptp_of_frame(std::uint64_t count) const noexcept -> std::int64_t { return ptp_of(count); }

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

    // No pairs: holdover once the reference times out, on the last line (published unlocked), for 5 s.
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
        if (unlocked_at == 0) {  // one publish, entering holdover: the same line, no longer locked
            EXPECT_EQ(changed, now - silent_at == holdover_at);
            Timebase held = last;
            held.locked = status.locked;
            EXPECT_TRUE(fit.timebase() == held);
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

//
// A media clock follower (follow_phase) - IEEE 1722-2016 Clause 4.4.4.3
//

namespace {

/// The distance of domain time `d` from the nearest 48 kHz frame, ns.
auto off_frame_grid_ns(std::int64_t const d) -> std::int64_t
{
    auto const clock = PtpMediaClock::make(48000);
    auto const t = static_cast<std::uint64_t>(d);
    std::uint64_t const below = clock->index_at(t);
    auto const low = static_cast<std::int64_t>(clock->time_of(below));
    auto const high = static_cast<std::int64_t>(clock->time_of(below + 1));
    return std::min(d - low, high - d);
}

/// Passes with the source silent for `duration_ns` of PTP time.
auto silent_for(TimebaseEstimator& fit, Source& source, std::int64_t& now, std::int64_t const duration_ns) -> void
{
    std::int64_t const end = now + duration_ns;
    while (now < end) {
        now += PASS_NS;
        source.skip_until(now);
        (void)fit.update(now);
    }
}

}  // namespace

// Before any pair the follower already runs, on PTP time itself, and its first pass publishes that.
TEST(mediaclock_timebase, a_phase_follower_runs_before_its_first_lock)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf_follower()};
    EXPECT_TRUE(fit.timebase().running);
    EXPECT_TRUE(fit.update(NOW_NS));
    EXPECT_FALSE(fit.update(NOW_NS + PASS_NS));
    auto const line = fit.timebase();
    EXPECT_TRUE(line.running && line.rate_offset == 0.0 && line.restarts == 0);
    EXPECT_EQ(line.to_domain(NOW_NS), NOW_NS);
    EXPECT_FALSE(fit.status().locked || fit.status().holdover);
}

// A CRF 7.7 us off the PTP grid and 12 ppm fast: once locked, each of its events is a whole number of 48 kHz frames
// of domain time (the join moved domain time by under half a frame), and the domain runs at its rate.
TEST(mediaclock_timebase, a_phase_follower_lands_the_source_frames_on_its_grid)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf_follower()};
    Source source{48000, 96, 12e-6, 200};
    source.step(-7'700);
    std::int64_t now = NOW_NS;
    Continuity continuity;
    run_for(fit, source, now, 3'000 * MS, [&](Timebase const& before, bool) { continuity.observe(before, fit.timebase(), now); });
    EXPECT_TRUE(fit.status().locked);
    EXPECT_TRUE(std::fabs(fit.status().rate_offset_ppm - 12.0) < 0.1);
    EXPECT_TRUE(continuity.max_publish_jump <= static_cast<std::int64_t>(SAMPLE_NS / 2) + 1);
    std::int64_t worst = 0;
    for (std::uint64_t event = 0; event < 1'000; ++event) {
        std::int64_t const ptp = source.ptp_of_frame((event * 96) + 96'000);
        worst = std::max(worst, off_frame_grid_ns(fit.timebase().to_domain(ptp)));
    }
    EXPECT_TRUE(worst < 100);
    EXPECT_EQ(fit.timebase().restarts, std::uint16_t{1});
}

// A follower that loses its reference keeps running on the last line for good, its loss one restart.
TEST(mediaclock_timebase, a_phase_follower_holds_over_without_end)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf_follower()};
    Source source{48000, 96, -30e-6, 200};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 1'000 * MS);
    auto const locked = fit.timebase();
    EXPECT_TRUE(fit.status().locked && locked.restarts == 1);
    silent_for(fit, source, now, 60'000 * MS);
    auto const held = fit.timebase();
    EXPECT_TRUE(held.running && fit.status().holdover && !fit.status().reference_present);
    EXPECT_EQ(held.restarts, std::uint16_t{2});
    EXPECT_TRUE(held.to_domain(now) == locked.to_domain(now) && held.rate_offset == locked.rate_offset);
}

// Each stimulus is one restart: the first lock, the CRF stopping, the CRF back in a new epoch (a restart or a
// rebind), and a toggle of its mr (a new epoch while it keeps coming: no loss, one re-lock).
TEST(mediaclock_timebase, each_lock_and_each_loss_is_one_restart)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf_follower()};
    Source source{48000, 96, 5e-6, 200};
    std::int64_t now = NOW_NS;
    std::vector<std::uint16_t> seen;
    auto const watch = [&](Timebase const& before, bool changed) {
        EXPECT_TRUE(changed || fit.timebase().restarts == before.restarts);
        if (fit.timebase().restarts != before.restarts) {
            seen.push_back(fit.timebase().restarts);
        }
    };
    run_for(fit, source, now, 2'000 * MS, watch);
    silent_for(fit, source, now, 2'000 * MS);
    seen.push_back(fit.timebase().restarts);
    source.restart(48000, 96, 5e-6);
    run_for(fit, source, now, 2'000 * MS, watch);
    source.restart(48000, 96, 5e-6);
    bool absent = false;
    run_for(fit, source, now, 2'000 * MS, [&](Timebase const& before, bool changed) {
        absent = absent || !fit.status().reference_present;
        watch(before, changed);
    });
    EXPECT_FALSE(absent);
    EXPECT_TRUE(seen == (std::vector<std::uint16_t>{1, 2, 3, 4}));
}

// Leaving the CRF (another clock source selected) joins a line at PTP's rate where the domain is: one restart,
// published at the next pass, and no lock until pairs are fed again. Unlocked, it is nothing to leave.
TEST(mediaclock_timebase, free_running_returns_to_the_reference_rate_with_one_restart)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf_follower()};
    fit.free_run(NOW_NS);
    EXPECT_TRUE(fit.update(NOW_NS));
    EXPECT_EQ(fit.timebase().restarts, std::uint16_t{0});
    Source source{48000, 96, 40e-6, 200};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 1'000 * MS);
    auto const followed = fit.timebase();
    EXPECT_TRUE(fit.status().locked && followed.restarts == 1);
    fit.free_run(now);
    EXPECT_TRUE(fit.update(now));
    auto const free = fit.timebase();
    EXPECT_TRUE(free.running && free.rate_offset == 0.0 && free.restarts == 2 && free.epoch == followed.epoch + 1);
    EXPECT_EQ(free.to_domain(now), followed.to_domain(now));
    fit.free_run(now + PASS_NS);
    silent_for(fit, source, now, 1'000 * MS);
    EXPECT_TRUE(fit.timebase() == free && !fit.status().locked && !fit.status().holdover);
}

// A CRF that comes back too briefly to lock and goes again is no second loss, and a follower never re-anchors
// (follow_phase clears absolute): its line runs on through a new epoch of the source.
TEST(mediaclock_timebase, a_brief_return_that_never_locks_is_no_restart)
{
    TimebaseEstimator fit{TimebaseEstimatorParams{.window_pairs = 1024, .lock_pairs = 128, .absolute = true, .follow_phase = true}};
    EXPECT_FALSE(fit.params().absolute);
    Source source{48000, 96, 5e-6, 200};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 1'000 * MS);
    silent_for(fit, source, now, 1'000 * MS);
    EXPECT_EQ(fit.timebase().restarts, std::uint16_t{2});
    source.restart(48000, 96, 5e-6);
    run_for(fit, source, now, 150 * MS);
    EXPECT_FALSE(fit.status().locked);
    silent_for(fit, source, now, 1'000 * MS);
    EXPECT_EQ(fit.timebase().restarts, std::uint16_t{2});
    EXPECT_TRUE(fit.timebase().running);
}

// A follower that loses lock to a noisy spell and regains it on the same source, in place, counts no restart: its
// line did not move, so no talker toggles mr.
TEST(mediaclock_timebase, a_lock_regained_in_place_is_no_restart)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf_follower()};
    Source source{48000, 96, 5e-6, 200};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 2'000 * MS);
    EXPECT_TRUE(fit.status().locked && fit.timebase().restarts == 1);
    source.set_jitter(4'000);  // an RMS residual just past lock_rms_ns, steps under step_threshold_ns
    bool lost = false;
    run_for(fit, source, now, 3'000 * MS, [&](Timebase const&, bool) { lost = lost || fit.status().holdover; });
    source.set_jitter(200);
    run_for(fit, source, now, 4'000 * MS);
    EXPECT_TRUE(lost && fit.status().locked && fit.status().reference_present);
    EXPECT_EQ(fit.timebase().restarts, std::uint16_t{1});
}

// A fit that is no follower counts no restarts: its joins keep domain time continuous, and free_run() leaves it as
// it is.
TEST(mediaclock_timebase, a_continuous_fit_counts_no_restarts)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf()};
    Source source{48000, 96, 5e-6, 200};
    std::int64_t now = NOW_NS;
    EXPECT_FALSE(fit.timebase().running);
    run_for(fit, source, now, 1'000 * MS);
    silent_for(fit, source, now, 1'000 * MS);
    source.restart(48000, 96, 5e-6);
    auto const before = fit.timebase();
    fit.free_run(now);
    EXPECT_TRUE(fit.timebase() == before && !fit.update(now));
    run_for(fit, source, now, 1'000 * MS);
    EXPECT_TRUE(fit.status().locked);
    EXPECT_EQ(fit.timebase().epoch, std::uint32_t{2});
    EXPECT_EQ(fit.timebase().restarts, std::uint16_t{0});
}

//
// Sender reports at any cadence - RFC 3550 Section 6.4.1, VSF TR-10-1 Section 8.10.1
//

namespace {

/// The preset of one report a second before window_span_ns: 16 pairs, locked on 4 whatever they span; its timeout
/// the 3 s that three of those intervals give rtcp_sender_reports(), its step the 3 reports that span 2 s.
constexpr TimebaseEstimatorParams ONE_A_SECOND{
    .window_pairs = 16, .lock_pairs = 4, .lock_span_ns = 0, .step_pairs = 3, .reference_timeout_ns = 3'000'000'000};

/// rtcp_sender_reports() with domain time the source's own, so a test compares the line with the source's truth.
constexpr auto absolute_reports() noexcept -> TimebaseEstimatorParams
{
    auto params = TimebaseEstimatorParams::rtcp_sender_reports();
    params.absolute = true;
    return params;
}

/// Uniform jitter of this half-width has an RMS of 1 us.
constexpr std::int64_t ONE_US_RMS_JITTER_NS = 1'732;

}  // namespace

// A pair 1 s older than the newest leaves; pairs 2 s apart still keep MIN_SPAN_PAIRS of them.
TEST(mediaclock_timebase, a_window_span_drops_pairs_older_than_it)
{
    TimebaseEstimatorParams const params{
        .window_pairs = 2048,
        .window_span_ns = 1'000'000'000,
        .lock_pairs = 4,
        .lock_span_ns = 0,
        .reference_timeout_ns = 5'000'000'000};
    TimebaseEstimator dense{params};
    Source every_10_ms{48000, 480, 0.0, 0};
    std::int64_t now = NOW_NS;
    run_for(dense, every_10_ms, now, 5'000 * MS);
    EXPECT_EQ(dense.status().pairs, std::uint32_t{101});
    EXPECT_TRUE(dense.status().locked);
    TimebaseEstimator sparse{params};
    Source every_2_s{48000, 96000, 0.0, 0};
    now = NOW_NS;
    run_for(sparse, every_2_s, now, 60'000 * MS);
    EXPECT_EQ(sparse.status().pairs, std::uint32_t{TimebaseEstimator::MIN_SPAN_PAIRS});
    EXPECT_TRUE(sparse.status().locked);
}

// One report a second, a step and a silence: the preset locks, steps and publishes as the 16-pair one did.
TEST(mediaclock_timebase, sender_reports_at_one_a_second_lock_as_before)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::rtcp_sender_reports()};
    TimebaseEstimator before{ONE_A_SECOND};
    Source source{48000, 48000, -100e-6, 0};
    Source same{48000, 48000, -100e-6, 0};
    std::int64_t now = NOW_NS;
    std::int64_t worst = 0;
    bool agree = true;
    auto const pass = [&](std::int64_t const duration_ns) {
        for (std::int64_t const end = now + duration_ns; now < end;) {
            now += PASS_NS;
            source.feed_until(fit, now);
            same.feed_until(before, now);
            agree = agree && fit.update(now) == before.update(now) && fit.status().locked == before.status().locked;
            worst = std::max(worst, std::abs(fit.timebase().to_domain(now) - before.timebase().to_domain(now)));
        }
    };
    pass(30'000 * MS);
    source.step(100'000);
    same.step(100'000);
    pass(30'000 * MS);
    source.skip_until(now + (8'000 * MS));
    same.skip_until(now + (8'000 * MS));
    pass(30'000 * MS);
    EXPECT_TRUE(agree && fit.status().locked && fit.status().steps == 1);
    EXPECT_TRUE(worst <= 1);
}

// IPMX's cadence: 4 reports would do, but the lock waits until they span 2.5 s; the window then holds 15 s of them.
TEST(mediaclock_timebase, a_hundred_reports_a_second_lock_within_three_seconds)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::rtcp_sender_reports()};
    Source source{48000, 480, 37e-6, 1'000};
    std::int64_t now = NOW_NS;
    std::int64_t const locked_at = time_to_lock(fit, source, now, 10'000 * MS);
    EXPECT_TRUE(locked_at >= 2'500 * MS && locked_at <= 2'520 * MS);
    run_for(fit, source, now, 20'000 * MS);
    auto const status = fit.status();
    EXPECT_TRUE(status.locked && status.pairs >= 1'499 && status.pairs <= 1'502);
    EXPECT_TRUE(std::fabs(status.rate_offset_ppm - 37.0) < 0.01);
}

// Reports with 1 us RMS of noise: the line at each pass is, in RMS, within a tenth of that of the sender's time.
TEST(mediaclock_timebase, a_hundred_noisy_reports_a_second_predict_within_a_tenth_of_their_noise)
{
    TimebaseEstimator fit{absolute_reports()};
    Source source{48000, 480, -23e-6, ONE_US_RMS_JITTER_NS};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 20'000 * MS);
    double sum_squares = 0.0;
    int passes = 0;
    run_for(fit, source, now, 60'000 * MS, [&](Timebase const&, bool) {
        double const error = static_cast<double>(fit.timebase().to_domain(now)) - source.source_ns_at(now);
        sum_squares += error * error;
        ++passes;
    });
    EXPECT_TRUE(fit.status().locked);
    EXPECT_TRUE(std::sqrt(sum_squares / passes) < 100.0);
}

// The same noise moves the line by a few nanoseconds at each publish: a listener's lock tolerance is microseconds.
TEST(mediaclock_timebase, a_publish_at_a_hundred_reports_a_second_moves_the_line_by_nanoseconds)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::rtcp_sender_reports()};
    Source source{48000, 480, 41e-6, ONE_US_RMS_JITTER_NS};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 5'000 * MS);
    Continuity continuity;
    int publishes = 0;
    run_for(fit, source, now, 60'000 * MS, [&](Timebase const& before, bool changed) {
        continuity.observe(before, fit.timebase(), now);
        publishes += changed ? 1 : 0;
    });
    EXPECT_TRUE(fit.status().locked && publishes > 5'000);
    EXPECT_TRUE(continuity.max_publish_jump < 20);
}

// A report every 4 s: locked after the fourth, the window then holding 16 of them (a minute).
TEST(mediaclock_timebase, a_report_every_four_seconds_locks_after_four)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::rtcp_sender_reports()};
    Source source{48000, 192000, 20e-6, 1'000};
    std::int64_t now = NOW_NS;
    std::int64_t const locked_at = time_to_lock(fit, source, now, 30'000 * MS);
    EXPECT_TRUE(locked_at > 11'990 * MS && locked_at <= 12'000 * MS);
    run_for(fit, source, now, 60'000 * MS);
    EXPECT_TRUE(fit.status().locked && fit.status().pairs == TimebaseEstimator::MIN_SPAN_PAIRS);
    EXPECT_TRUE(std::fabs(fit.status().rate_offset_ppm - 20.0) < 0.1);
}

// A follower switched from a CRF to sender reports keeps its line, epoch and restarts, then follows the reports.
TEST(mediaclock_timebase, a_retuned_follower_keeps_its_line_and_its_restarts)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf_follower()};
    Source crf{48000, 96, 12e-6, 200};
    std::int64_t now = NOW_NS;
    run_for(fit, crf, now, 1'000 * MS);
    auto const followed = fit.timebase();
    fit.retune(now, TimebaseEstimatorParams::rtcp_sender_reports());  // left at now: one restart, no stale line
    EXPECT_TRUE(fit.update(now));
    auto const left = fit.timebase();
    EXPECT_TRUE(left.restarts == followed.restarts + 1 && left.rate_offset == 0.0 && !left.locked);
    EXPECT_EQ(left.to_domain(now), followed.to_domain(now));
    EXPECT_FALSE(fit.update(now));
    auto const& params = fit.params();
    EXPECT_TRUE(params.follow_phase && !params.absolute && params.window_span_ns == 15'000'000'000);
    EXPECT_TRUE(params.window_pairs == 2048 && params.lock_pairs == 4 && params.step_pairs == 2);
    Source reports{48000, 480, -8e-6, 200};
    reports.restart(48000, 480, -8e-6);
    run_for(fit, reports, now, 5'000 * MS);
    EXPECT_TRUE(fit.status().locked && std::fabs(fit.status().rate_offset_ppm + 8.0) < 0.1);
    EXPECT_TRUE(fit.timebase().restarts == left.restarts + 1 && fit.timebase().epoch == left.epoch + 1);
}

// A fit that is no follower keeps its preset, window and line: a source switch rebuilds it instead.
TEST(mediaclock_timebase, a_retune_of_a_fit_that_does_not_follow_changes_nothing)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::crf()};
    Source source{48000, 96, 5e-6, 200};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 1'000 * MS);
    auto const line = fit.timebase();
    auto const pairs = fit.status().pairs;
    fit.retune(now, TimebaseEstimatorParams::rtcp_sender_reports());
    EXPECT_TRUE(fit.timebase() == line && fit.status().pairs == pairs && fit.status().locked);
    EXPECT_TRUE(fit.params().window_pairs == 1024 && fit.params().lock_pairs == 128 && !fit.params().follow_phase);
}

// A sender 9 us off the PTP grid and 30 ppm slow: once followed, each report's frame is on the domain's grid.
TEST(mediaclock_timebase, a_follower_of_sender_reports_lands_their_frames_on_its_grid)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::follower(TimebaseEstimatorParams::rtcp_sender_reports())};
    Source source{48000, 480, -30e-6, 0};
    source.step(9'000);
    std::int64_t now = NOW_NS;
    Continuity continuity;
    run_for(fit, source, now, 5'000 * MS, [&](Timebase const& before, bool) { continuity.observe(before, fit.timebase(), now); });
    EXPECT_TRUE(fit.status().locked && fit.timebase().restarts == 1);
    EXPECT_TRUE(continuity.max_publish_jump <= static_cast<std::int64_t>(SAMPLE_NS / 2) + 1);
    std::int64_t worst = 0;
    for (std::uint64_t report = 0; report < 1'000; ++report) {
        std::int64_t const ptp = source.ptp_of_frame((report * 480) + 480'000);
        worst = std::max(worst, off_frame_grid_ns(fit.timebase().to_domain(ptp)));
    }
    EXPECT_TRUE(worst < 100);
}

// Reports that stop for over 1 s (three intervals, at least 1 s): the follower runs on its line, one restart; their
// return is another.
TEST(mediaclock_timebase, a_follower_that_loses_its_reports_holds_its_line_and_counts_a_restart)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::follower(TimebaseEstimatorParams::rtcp_sender_reports())};
    Source source{48000, 480, 15e-6, 200};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 5'000 * MS);
    auto const locked = fit.timebase();
    silent_for(fit, source, now, 900 * MS);
    EXPECT_TRUE(fit.status().reference_present && fit.timebase().restarts == 1);
    silent_for(fit, source, now, 200 * MS);
    auto const held = fit.timebase();
    EXPECT_TRUE(held.running && fit.status().holdover && held.restarts == 2);
    EXPECT_TRUE(held.to_domain(now) == locked.to_domain(now) && held.rate_offset == locked.rate_offset);
    run_for(fit, source, now, 5'000 * MS);
    EXPECT_TRUE(fit.status().locked && fit.timebase().restarts == 3);
}

// RFC 3550's randomised interval (Section 6.3.1: Td of 5 s times 0.5 to 1.5, over 1.21828), an hour of it: one lock
// and no loss, the timeout following the sender's own cadence.
TEST(mediaclock_timebase, a_randomised_rfc_3550_sender_locks_once_and_stays_locked)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::follower(TimebaseEstimatorParams::rtcp_sender_reports())};
    constexpr double TD_FRAMES = 5.0 * 48000.0;
    constexpr double RATE_OFFSET = 17e-6;
    std::uint32_t rng = 0x2468ACE1U;
    std::uint64_t count = 0;
    auto const ptp_of = [](std::uint64_t const frames) {
        return NOW_NS + std::llround(static_cast<double>(frames) * 1e9 / (48000.0 * (1.0 + RATE_OFFSET)));
    };
    int locks = 0;
    bool was_locked = false;
    for (std::int64_t now = NOW_NS; now < NOW_NS + (3'600'000 * MS); now += PASS_NS) {
        for (; ptp_of(count) <= now;) {
            fit.feed(ClockSample{static_cast<std::uint64_t>(ptp_of(count)), count, 48000, 1});
            rng ^= rng << 13U;
            rng ^= rng >> 17U;
            rng ^= rng << 5U;
            double const draw = 0.5 + (static_cast<double>(rng) / 4294967296.0);
            count += static_cast<std::uint64_t>(std::llround(TD_FRAMES * draw / 1.21828));
        }
        (void)fit.update(now);
        bool const locked = fit.status().locked;
        locks += locked && !was_locked ? 1 : 0;
        was_locked = locked;
    }
    EXPECT_EQ(locks, 1);
    EXPECT_TRUE(was_locked && fit.timebase().restarts == 1);
}

// At IPMX's cadence a 20 ms excursion of 50 us in the reports' times (a sender's hiccup: two reports) is two outliers,
// not a step: the follower's line and restarts stay; a lasting 50 us step is one, confirmed once it spans 2 s.
TEST(mediaclock_timebase, a_twenty_millisecond_excursion_at_a_hundred_reports_a_second_is_no_step)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::follower(TimebaseEstimatorParams::rtcp_sender_reports())};
    Source source{48000, 480, 12e-6, 500};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 5'000 * MS);
    auto const before = fit.timebase();
    EXPECT_TRUE(fit.status().locked && before.restarts == 1);
    source.outliers(2, 50'000);
    run_for(fit, source, now, 5'000 * MS);
    EXPECT_TRUE(fit.status().locked && fit.status().steps == 0 && fit.status().outliers == 2);
    EXPECT_TRUE(fit.timebase().restarts == before.restarts && fit.timebase().epoch == before.epoch);
    source.step(-50'000);
    std::int64_t const stepped = now;
    run_for(fit, source, now, 1'900 * MS);
    EXPECT_EQ(fit.status().steps, std::uint32_t{0});
    run_for(fit, source, now, 6'000 * MS);
    EXPECT_TRUE(fit.status().steps == 1 && fit.status().last_step_ns >= stepped && fit.status().locked);
    EXPECT_EQ(fit.timebase().restarts, static_cast<std::uint16_t>(before.restarts + 1));
}

// The published line says whether the fit is locked: not before the first lock or in holdover, though a follower's
// line runs throughout.
TEST(mediaclock_timebase, the_line_says_whether_the_fit_is_locked)
{
    TimebaseEstimator fit{TimebaseEstimatorParams::follower(TimebaseEstimatorParams::rtcp_sender_reports())};
    Source source{48000, 480, 12e-6, 500};
    std::int64_t now = NOW_NS;
    run_for(fit, source, now, 1'000 * MS);
    EXPECT_TRUE(fit.timebase().running && !fit.timebase().locked);
    run_for(fit, source, now, 3'000 * MS);
    EXPECT_TRUE(fit.timebase().locked);
    silent_for(fit, source, now, 1'500 * MS);
    EXPECT_TRUE(fit.timebase().running && !fit.timebase().locked && fit.status().holdover);
}

TEST_MAIN(padl_mediaclock, mediaclock_timebase_test)
