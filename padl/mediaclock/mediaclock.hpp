#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// Media clock module header - the RFC 7273 media clock and the clock domains fitted against PTP.
///
/// Bundles PtpMediaClock (sample index to PTP time, RTP and AVTP timestamps) and the Timebase
/// and TimebaseEstimator of a clock domain.

#include "padl/mediaclock/mediaclock_ptp_clock.hpp"
#include "padl/mediaclock/mediaclock_timebase.hpp"
