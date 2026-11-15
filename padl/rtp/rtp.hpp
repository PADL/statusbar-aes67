#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RTP module header - RFC 3550 RTP and RTCP as AES67 streams carry them.
///
/// Bundles the RTP header, sequence validation and raw frames, the RTCP packets with the IPMX
/// Info Block, the RTCP time conversions and interval, and the module's errors. Include the
/// specific sub-header when only one is needed.

#include "padl/rtp/rtp_error.hpp"
#include "padl/rtp/rtp_frame.hpp"
#include "padl/rtp/rtp_header.hpp"
#include "padl/rtp/rtp_rtcp_interval.hpp"
#include "padl/rtp/rtp_rtcp_ipmx.hpp"
#include "padl/rtp/rtp_rtcp_packet.hpp"
#include "padl/rtp/rtp_rtcp_time.hpp"
#include "padl/rtp/rtp_sequence.hpp"
