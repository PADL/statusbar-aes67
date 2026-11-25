#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// What an AES67 stream is, as a transport is asked to carry it: the ReceivePlan a Receiver makes from the SDP, the
/// SendPlan a Sender makes from its destination, and the SendRun where each of a sender's runs starts. Plain values.

#include "padl/aes67/aes67_stream.hpp"
#include "padl/rtp/rtp_frame.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/net/net_socket.hpp"

#include <cstdint>
#include <optional>

namespace padl::aes67 {

/// A stream to receive: where it arrives, and how its packets are admitted (Depacketizer) and placed.
struct ReceivePlan
{
    statusbar::net::IPv4Endpoint destination;  ///< a group, or an address of this host, and the UDP port
    /// The source a source-specific group is joined for (RFC 4607 Section 4.1): the SDP's source filter for the
    /// destination (RFC 4570); nullopt: any source.
    std::optional<statusbar::ip::IPv4Address> join_source;
    StreamFormat format;
    uint8_t payload_type{0};
    uint32_t max_frames{0};  ///< the most frames a packet holds: its ptime's, or its maxptime's when longer
    /// The one source address admitted: the source filter's, or an offer's sender (AES67-2023 Clause 8.5.2); nullopt: any.
    std::optional<statusbar::ip::IPv4Address> source_filter;
    std::optional<uint32_t> offset;  ///< the RTP offset of a direct media clock (RFC 7273 Section 5.2); nullopt: none yet
    bool sender_clock{false};        ///< `mediaclk:sender`: the offset comes from its sender's reports
    /// The PTP grandmaster and domain the stream's ts-refclk names (named_grandmaster(), RFC 7273 Section 4.8); nullopt:
    /// it names none.
    std::optional<Grandmaster> grandmaster;

    auto operator==(ReceivePlan const&) const noexcept -> bool = default;
};

/// A stream to send: where it goes, how it is marked, and its packets (PacketSchedule's grid).
struct SendPlan
{
    statusbar::net::IPv4Endpoint destination;        ///< a group or a unicast address, and the UDP port
    uint8_t ttl{32};                                 ///< the IPv4 TTL
    uint8_t dscp{rtp::FrameAddressing::DSCP_MEDIA};  ///< the IPv4 DSCP
    std::optional<statusbar::ieee::VlanTag> vlan;    ///< the 802.1Q tag; none: untagged
    StreamFormat format;
    uint8_t payload_type{96};
    uint32_t phase{0};  ///< where the packet grid starts, in frames modulo frames_per_packet
    /// The PTP grandmaster and domain its ts-refclk names (Sender::grandmaster()) when it is opened; nullopt: not known.
    /// One learnt after configure() reaches a plan only at the next configure(): the Sender reopens nothing for it.
    std::optional<Grandmaster> grandmaster;

    auto operator==(SendPlan const&) const noexcept -> bool = default;
};

/// Where one of a sender's runs starts (RFC 3550 Section 8: each run its own SSRC and sequence).
struct SendRun
{
    uint32_t ssrc{0};
    uint16_t first_sequence{0};
    uint64_t first_index{0};  ///< the first packet's first frame, on the plan's grid
    uint32_t send_offset{0};  ///< frames: a packet is due once its last frame is this far behind now

    auto operator==(SendRun const&) const noexcept -> bool = default;
};

}  // namespace padl::aes67
