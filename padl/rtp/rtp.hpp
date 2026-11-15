#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RTP module header (RFC 3550): the RTP header, sequence validation and raw frames, RTCP with the
/// IPMX Info Block, its time and interval, and the errors. Include a sub-header for only one.

#include "padl/rtp/rtp_error.hpp"
#include "padl/rtp/rtp_frame.hpp"
#include "padl/rtp/rtp_header.hpp"
#include "padl/rtp/rtp_rtcp_interval.hpp"
#include "padl/rtp/rtp_rtcp_ipmx.hpp"
#include "padl/rtp/rtp_rtcp_packet.hpp"
#include "padl/rtp/rtp_rtcp_time.hpp"
#include "padl/rtp/rtp_sequence.hpp"
