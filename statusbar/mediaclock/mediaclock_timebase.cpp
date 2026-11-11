// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/mediaclock/mediaclock_timebase.hpp"

#include "statusbar/status/statusbar_assert.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace statusbar::mediaclock {

namespace {

constexpr std::uint64_t NS_PER_SECOND = 1'000'000'000;

/// Source time of frame `count` at `rate_hz`, rounded to the nanosecond, without overflow.
[[nodiscard]] constexpr auto source_ns_of(std::uint64_t count, std::uint32_t rate_hz) noexcept -> std::int64_t
{
    std::uint64_t const seconds = count / rate_hz;
    std::uint64_t const frames = count % rate_hz;
    return static_cast<std::int64_t>((seconds * NS_PER_SECOND) + (((frames * NS_PER_SECOND) + (rate_hz / 2)) / rate_hz));
}

/// `ns` rounded to the nearest whole frame at `rate_hz`, without overflow: a domain constant on the frame grid.
[[nodiscard]] constexpr auto on_frame_grid(std::int64_t const ns, std::uint32_t const rate_hz) noexcept -> std::int64_t
{
    if (rate_hz == 0) {
        return ns;
    }
    if (ns < 0) {
        return -on_frame_grid(-ns, rate_hz);
    }
    auto const whole = static_cast<std::uint64_t>(ns);
    std::uint64_t const frames =
        ((whole / NS_PER_SECOND) * rate_hz) + ((((whole % NS_PER_SECOND) * rate_hz) + (NS_PER_SECOND / 2)) / NS_PER_SECOND);
    return source_ns_of(frames, rate_hz);
}

}  // namespace

auto TimebaseEstimator::clamped(TimebaseEstimatorParams params) noexcept -> TimebaseEstimatorParams
{
    params.window_pairs = std::clamp<std::uint32_t>(params.window_pairs, 2, MAX_WINDOW_PAIRS);
    params.step_pairs = std::clamp<std::uint32_t>(params.step_pairs, 1, MAX_STEP_PAIRS);
    params.lock_pairs = std::clamp<std::uint32_t>(params.lock_pairs, 2, params.window_pairs);
    params.absolute = params.absolute && !params.follow_phase;  // a follower joins, it never re-anchors
    return params;
}

TimebaseEstimator::TimebaseEstimator(TimebaseEstimatorParams const& params) noexcept
    : params_{clamped(params)}
{
    // A follower runs from the start, on the identity line: the first update() publishes it.
    published_.running = params_.follow_phase;
    publish_pending_ = params_.follow_phase;
}

auto TimebaseEstimator::pair_at(std::size_t i) const noexcept -> Pair const&
{
    return window_[(head_ + i) % params_.window_pairs];
}

auto TimebaseEstimator::check_pairs() const noexcept -> std::size_t
{
    return std::max<std::size_t>(3, params_.lock_pairs / 8);
}

template <class F>
auto TimebaseEstimator::for_each_pair(F const& f) const noexcept -> void
{
    std::size_t const first = std::min<std::size_t>(size_, params_.window_pairs - head_);
    for (std::size_t i = 0; i < first; ++i) {
        f(window_[head_ + i]);
    }
    for (std::size_t i = 0; i < size_ - first; ++i) {
        f(window_[i]);
    }
}

auto TimebaseEstimator::push(Pair const& p) noexcept -> void
{
    STATUSBAR_ASSERT(params_.window_pairs >= 2);  // clamped by the constructor
    if (size_ == params_.window_pairs) {
        head_ = (head_ + 1) % params_.window_pairs;
        --size_;
    }
    window_[(head_ + size_) % params_.window_pairs] = p;
    ++size_;
    ++pending_;
    trim_span();
}

auto TimebaseEstimator::trim_span() noexcept -> void
{
    if (params_.window_span_ns <= 0) {
        return;
    }
    std::size_t const floor = std::max<std::size_t>(params_.lock_pairs, MIN_SPAN_PAIRS);
    while (size_ > floor && (newest().reference_ns - pair_at(0).reference_ns) > params_.window_span_ns) {
        head_ = (head_ + 1) % params_.window_pairs;
        --size_;
    }
}

auto TimebaseEstimator::reset_window() noexcept -> void
{  // NOLINT(statusbar-block-statements): the window's fields reset together
    head_ = 0;
    size_ = 0;
    pending_ = 0;
    held_count_ = 0;
    fit_valid_ = false;
    fit_pairs_ = 0;
    if (state_ == State::locked) {
        state_ = State::holdover;
    }
}

auto TimebaseEstimator::source_discontinuity() noexcept -> void
{
    reset_window();
    source_restarted_ = true;  // a follower's next lock is a restart even where it lands
    if (params_.absolute && state_ != State::unlocked) {
        state_ = State::unlocked;  // the last line is no longer the source's time
        published_.running = false;
        publish_pending_ = true;
    }
}

auto TimebaseEstimator::fitted_source_ns(std::int64_t t_phc) const noexcept -> std::int64_t
{
    return anchor_source_ns_ + std::llround(intercept_ + (slope_ * static_cast<double>(t_phc - anchor_reference_ns_)));
}

auto TimebaseEstimator::residual(Pair const& p) const noexcept -> double
{
    double const ds = static_cast<double>(p.source_ns - anchor_source_ns_);
    return ds - intercept_ - (slope_ * static_cast<double>(p.reference_ns - anchor_reference_ns_));
}

auto TimebaseEstimator::track_source(ClockSample const& sample) noexcept -> void
{
    if (have_source_ && (sample.epoch != source_epoch_ || sample.rate_hz != source_rate_hz_)) {
        outliers_ += static_cast<std::uint32_t>(held_count_);
        source_discontinuity();
    }
    have_source_ = true;
    source_epoch_ = sample.epoch;
    source_rate_hz_ = sample.rate_hz;
}

auto TimebaseEstimator::accept_reference(std::int64_t const reference_ns) noexcept -> bool
{
    if (have_reference_ && reference_ns <= last_reference_ns_) {
        return false;
    }
    have_reference_ = true;
    last_reference_ns_ = reference_ns;
    return true;
}

auto TimebaseEstimator::hold(Pair const& p) noexcept -> void
{
    held_first_ns_ = held_count_ == 0 ? p.reference_ns : held_first_ns_;
    if (held_count_ < MAX_STEP_PAIRS) {
        held_[held_count_] = p;
    } else {
        std::shift_left(held_.begin(), held_.end(), 1);  // the newest kept, oldest first
        held_.back() = p;
    }
    ++held_count_;
    if (held_count_ >= params_.step_pairs && (p.reference_ns - held_first_ns_) >= params_.step_span_ns) {
        confirm_step();
    }
}

auto TimebaseEstimator::confirm_step() noexcept -> void
{
    if (fit_pairs_ >= params_.lock_pairs) {  // a shorter fit is restarted, not stepped
        ++steps_;
        last_step_ns_ = held_first_ns_;
    }
    std::size_t const n = std::min(held_count_, MAX_STEP_PAIRS);
    source_discontinuity();
    for (std::size_t i = 0; i < n; ++i) {
        push(held_[i]);
    }
}

auto TimebaseEstimator::screen_step(Pair const& p) noexcept -> bool
{
    if (!fit_valid_ || fit_pairs_ < check_pairs()) {
        return false;
    }
    if (std::abs(residual(p)) > static_cast<double>(params_.step_threshold_ns)) {
        hold(p);
        return true;
    }
    outliers_ += static_cast<std::uint32_t>(held_count_);  // the pairs held were outliers, not a step
    held_count_ = 0;
    return false;
}

auto TimebaseEstimator::feed(ClockSample const& sample) noexcept -> void
{
    if (sample.rate_hz == 0) {
        return;
    }
    track_source(sample);
    Pair const p{
        .reference_ns = static_cast<std::int64_t>(sample.reference_ns), .source_ns = source_ns_of(sample.count, sample.rate_hz)};
    if (!accept_reference(p.reference_ns) || screen_step(p)) {
        return;
    }
    push(p);
}

auto TimebaseEstimator::fit_line(Pair const& anchor) noexcept -> bool
{  // NOLINT(statusbar-block-statements): an ordinary least-squares fit, read as one derivation
    double const n = static_cast<double>(size_);
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for_each_pair([&](Pair const& p) {
        auto const x = static_cast<double>(p.reference_ns - anchor.reference_ns);
        auto const y = static_cast<double>(p.source_ns - anchor.source_ns);
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    });
    double const mean_x = sum_x / n;
    double const mean_y = sum_y / n;
    double const sxx = sum_xx - (n * mean_x * mean_x);
    double const sxy = sum_xy - (n * mean_x * mean_y);
    if (sxx <= 0.0) {
        return false;
    }
    slope_ = sxy / sxx;
    intercept_ = mean_y - (slope_ * mean_x);
    anchor_reference_ns_ = anchor.reference_ns;
    anchor_source_ns_ = anchor.source_ns;
    return true;
}

auto TimebaseEstimator::rms_residual() const noexcept -> double
{
    double rss = 0.0;
    for_each_pair([&](Pair const& p) {
        double const r = residual(p);
        rss += r * r;
    });
    return std::sqrt(rss / static_cast<double>(size_));
}

auto TimebaseEstimator::refit() noexcept -> void
{
    pending_ = 0;
    // Centre on the newest pair: integer subtraction first, so nothing loses a nanosecond.
    fit_valid_ = size_ >= 2 && fit_line(newest());
    if (fit_valid_) {
        rms_ns_ = rms_residual();
        fit_pairs_ = size_;
    }
}

auto TimebaseEstimator::meets_lock() const noexcept -> bool
{
    return fit_valid_ && fit_pairs_ >= params_.lock_pairs &&
        (newest().reference_ns - pair_at(0).reference_ns) >= params_.lock_span_ns &&
        rms_ns_ < static_cast<double>(params_.lock_rms_ns) && std::abs(slope_ - 1.0) < params_.lock_rate_ppm * 1e-6;
}

auto TimebaseEstimator::line() const noexcept -> Timebase
{
    return Timebase{
        .reference_ns = anchor_reference_ns_,
        .domain_ns = anchor_source_ns_ + std::llround(intercept_) + domain_constant_ns_,
        .rate_offset = slope_ - 1.0,
        .epoch = published_.epoch,
        .running = true,
        .locked = true,
        .restarts = published_.restarts,
    };
}

auto TimebaseEstimator::restart() noexcept -> void
{
    ++published_.restarts;
    publish_pending_ = true;
}

auto TimebaseEstimator::reference_timeout() const noexcept -> std::int64_t
{
    if (params_.timeout_intervals == 0) {
        return params_.reference_timeout_ns;
    }
    if (size_ < 2) {
        return params_.reference_timeout_max_ns;
    }
    std::int64_t const mean = (newest().reference_ns - pair_at(0).reference_ns) / static_cast<std::int64_t>(size_ - 1);
    std::int64_t const ceiling = std::max(params_.reference_timeout_max_ns, params_.reference_timeout_ns);
    return std::clamp(mean * params_.timeout_intervals, params_.reference_timeout_ns, ceiling);
}

auto TimebaseEstimator::check_reference(std::int64_t const now_ns) noexcept -> void
{
    bool const was_present = reference_present_;
    reference_present_ = have_reference_ && (now_ns - last_reference_ns_) <= reference_timeout();
    if (!reference_present_ && size_ != 0) {
        reset_window();
    }
    // A follower that loses the reference it followed restarts its media clock (on the line it holds).
    if (was_present && !reference_present_ && std::exchange(following_, false)) {
        restart();
    }
}

auto TimebaseEstimator::notify_follow_lock(std::int64_t const now_ns) noexcept -> void
{
    // The first lock after a loss or a source discontinuity restarts, as does one whose grid snap moves domain time
    // by over RESTART_NS; a lock regained on the same source in place does not.
    std::int64_t const moved = domain_constant_ns_ - (published_.to_domain(now_ns) - fitted_source_ns(now_ns));
    bool const stepped = moved > RESTART_NS || moved < -RESTART_NS;
    if (params_.follow_phase && (!following_ || source_restarted_ || stepped)) {
        restart();
    }
    following_ = params_.follow_phase;
    source_restarted_ = false;
}

auto TimebaseEstimator::join_constant(std::int64_t const now_ns) const noexcept -> std::int64_t
{
    if (params_.absolute) {
        return 0;  // the source's own time
    }
    std::int64_t const joined = published_.to_domain(now_ns) - fitted_source_ns(now_ns);
    return params_.follow_phase ? on_frame_grid(joined, source_rate_hz_) : joined;
}

auto TimebaseEstimator::publish_locked(std::int64_t const now_ns, bool const refitted) noexcept -> bool
{
    bool const joined = state_ != State::locked;
    if (joined) {
        // Join the new line to the one in use at now_ns (only the slope changes; a follower's phase by under half
        // a frame), or adopt the source's own time when absolute.
        domain_constant_ns_ = join_constant(now_ns);
        ++published_.epoch;
        notify_follow_lock(now_ns);
        state_ = State::locked;
    }
    if (!joined && !refitted) {
        return false;
    }
    last_good_ns_ = now_ns;
    published_ = line();
    return true;
}

auto TimebaseEstimator::lose_lock(std::int64_t const now_ns) noexcept -> bool
{
    if (state_ == State::locked) {
        state_ = State::holdover;
    }
    if (std::exchange(published_.locked, false)) {
        publish_pending_ = true;  // the line runs on, no longer locked to its source
    }
    if (state_ == State::holdover && !params_.follow_phase && (now_ns - last_good_ns_) > params_.holdover_ns) {
        state_ = State::unlocked;
        published_.running = false;
        return true;
    }
    return false;
}

auto TimebaseEstimator::update(std::int64_t now_ns) noexcept -> bool
{
    bool const refitted = pending_ != 0;
    if (refitted) {
        refit();
    }
    check_reference(now_ns);
    bool const changed = reference_present_ && meets_lock() ? publish_locked(now_ns, refitted) : lose_lock(now_ns);
    return std::exchange(publish_pending_, false) || changed;
}

auto TimebaseEstimator::free_run(std::int64_t const now_ns) noexcept -> void
{
    if (!params_.follow_phase) {
        return;  // only a follower leaves its source: another fit's line stays the fit's
    }
    bool const left = state_ != State::unlocked;
    forget_source();
    if (left) {
        leave_line(now_ns);
    }
}

auto TimebaseEstimator::retune(std::int64_t const now_ns, TimebaseEstimatorParams const& params) noexcept -> void
{
    if (!params_.follow_phase) {
        return;  // only a follower changes source kind without a new fit
    }
    free_run(now_ns);
    have_reference_ = false;  // the next source's pairs are on their own timeline
    have_source_ = false;
    params_ = clamped(TimebaseEstimatorParams::follower(params));
}

auto TimebaseEstimator::leave_line(std::int64_t const now_ns) noexcept -> void
{
    // The line in use at now_ns, on at the reference's own rate in a new epoch; the source must lock afresh.
    published_ = Timebase{
        .reference_ns = now_ns,
        .domain_ns = published_.to_domain(now_ns),
        .epoch = published_.epoch + 1,
        .running = true,
        .restarts = published_.restarts,
    };
    restart();
}

auto TimebaseEstimator::forget_source() noexcept -> void
{
    // The newest reference stays: a pair from before this is still refused (accept_reference).
    reset_window();
    state_ = State::unlocked;
    reference_present_ = false;
    following_ = false;
}

auto TimebaseEstimator::status() const noexcept -> TimebaseEstimatorStatus
{
    double const rate = fit_valid_ ? slope_ - 1.0 : published_.rate_offset;
    return TimebaseEstimatorStatus{
        .locked = state_ == State::locked,
        .holdover = state_ == State::holdover,
        .reference_present = reference_present_,
        .rate_offset_ppm = rate * 1e6,
        .residual_ns = fit_valid_ ? std::llround(rms_ns_) : 0,
        .last_step_ns = last_step_ns_,
        .steps = steps_,
        .outliers = outliers_,
        .pairs = static_cast<std::uint32_t>(size_),
    };
}

}  // namespace statusbar::mediaclock
