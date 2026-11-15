#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// AES67 module header - AES67 stream I/O.
///
/// Bundles the stream formats, the Sender and Receiver, their RTCP sessions, and the UDP and
/// AF_XDP transports with the socket helpers they share. Include the specific sub-header when
/// only one is needed.

#include "padl/aes67/aes67_error.hpp"
#include "padl/aes67/aes67_receiver.hpp"
#include "padl/aes67/aes67_rtcp.hpp"
#include "padl/aes67/aes67_sender.hpp"
#include "padl/aes67/aes67_socket.hpp"
#include "padl/aes67/aes67_stream.hpp"
#include "padl/aes67/aes67_transport.hpp"
#include "padl/aes67/aes67_udp_transport.hpp"
#include "padl/aes67/aes67_xdp_transport.hpp"
