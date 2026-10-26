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

}  // namespace

TimebaseEstimator::TimebaseEstimator(TimebaseEstimatorParams const& params) noexcept
    : params_{params}
{
    params_.window_pairs = std::clamp<std::uint32_t>(params_.window_pairs, 2, MAX_WINDOW_PAIRS);
    params_.step_pairs = std::clamp<std::uint32_t>(params_.step_pairs, 1, MAX_STEP_PAIRS);
    params_.lock_pairs = std::clamp<std::uint32_t>(params_.lock_pairs, 2, params_.window_pairs);
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
}

auto TimebaseEstimator::reset_window() noexcept -> void
{
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
    if (params_.absolute && state_ != State::unlocked) {
        state_ = State::unlocked;  // the last line is no longer the source's time
        published_.running = false;
        stop_pending_ = true;
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

auto TimebaseEstimator::feed(ClockSample const& sample) noexcept -> void
{
    if (sample.rate_hz == 0) {
        return;
    }
    if (have_source_ && (sample.epoch != source_epoch_ || sample.rate_hz != source_rate_hz_)) {
        outliers_ += static_cast<std::uint32_t>(held_count_);
        source_discontinuity();
    }
    have_source_ = true;
    source_epoch_ = sample.epoch;
    source_rate_hz_ = sample.rate_hz;

    Pair const p{
        .reference_ns = static_cast<std::int64_t>(sample.reference_ns), .source_ns = source_ns_of(sample.count, sample.rate_hz)};
    if (have_reference_ && p.reference_ns <= last_reference_ns_) {
        return;  // out of order or repeated
    }
    have_reference_ = true;
    last_reference_ns_ = p.reference_ns;
    if (fit_valid_ && fit_pairs_ >= check_pairs()) {
        if (std::abs(residual(p)) > static_cast<double>(params_.step_threshold_ns)) {
            held_[held_count_++] = p;
            if (held_count_ < params_.step_pairs) {
                return;
            }
            if (fit_pairs_ >= params_.lock_pairs) {  // a shorter fit is restarted, not stepped
                ++steps_;
                last_step_ns_ = held_[0].reference_ns;
            }
            std::size_t const n = held_count_;
            source_discontinuity();
            for (std::size_t i = 0; i < n; ++i) {
                push(held_[i]);
            }
            return;
        }
        outliers_ += static_cast<std::uint32_t>(held_count_);
        held_count_ = 0;
    }
    push(p);
}

auto TimebaseEstimator::refit() noexcept -> void
{
    pending_ = 0;
    if (size_ < 2) {
        fit_valid_ = false;
        return;
    }
    // Centre on the newest pair: integer subtraction first, so nothing loses a nanosecond.
    Pair const anchor = newest();
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
        fit_valid_ = false;
        return;
    }
    slope_ = sxy / sxx;
    intercept_ = mean_y - (slope_ * mean_x);
    anchor_reference_ns_ = anchor.reference_ns;
    anchor_source_ns_ = anchor.source_ns;

    double rss = 0.0;
    for_each_pair([&](Pair const& p) {
        double const r = residual(p);
        rss += r * r;
    });
    rms_ns_ = std::sqrt(rss / n);
    fit_valid_ = true;
    fit_pairs_ = size_;
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
    };
}

auto TimebaseEstimator::update(std::int64_t now_ns) noexcept -> bool
{
    bool const stopped = std::exchange(stop_pending_, false);
    bool const refitted = pending_ != 0;
    if (refitted) {
        refit();
    }
    reference_present_ = have_reference_ && (now_ns - last_reference_ns_) <= params_.reference_timeout_ns;
    if (!reference_present_ && size_ != 0) {
        reset_window();  // pairs from before a silence do not lock the line after it
    }

    if (reference_present_ && meets_lock()) {
        bool const joined = state_ != State::locked;
        if (joined) {
            // Join the new line to the one in use at now_ns (only the slope changes), or take the
            // source's own time when absolute.
            domain_constant_ns_ = params_.absolute ? 0 : published_.to_domain(now_ns) - fitted_source_ns(now_ns);
            ++published_.epoch;
            state_ = State::locked;
        }
        if (!joined && !refitted) {
            return false;
        }
        last_good_ns_ = now_ns;
        published_ = line();
        return true;
    }

    if (state_ == State::locked) {
        state_ = State::holdover;
    }
    if (state_ == State::holdover && (now_ns - last_good_ns_) > params_.holdover_ns) {
        state_ = State::unlocked;
        published_.running = false;
        return true;
    }
    return stopped;
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
