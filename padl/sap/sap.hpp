#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// SAP module header - RFC 2974 Session Announcement Protocol.
///
/// Bundles the packet codec, the announcer and the listening side (the session directory and
/// its receiver). Include the specific sub-header when only one is needed.

#include "padl/sap/sap_announcer.hpp"
#include "padl/sap/sap_packet.hpp"
#include "padl/sap/sap_receiver.hpp"
