#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// Test support: SAP packets as SapAnnouncer writes them, for the tests of SAP listeners (sap, the
/// bridge's control server).

#include "padl/sap/sap.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace padl::sap {

/// A version 1 SAP packet from `origin`: an announcement, or with `deletion` a deletion, of `sdp`.
[[nodiscard]] inline auto make_sap_packet(
    bool const deletion, uint16_t const hash, statusbar::ip::IPv4Address const& origin, std::string_view const sdp)
    -> std::vector<uint8_t>
{
    SapHeader header{};
    header.init(deletion, hash, origin);
    std::vector<uint8_t> packet(SapHeader::LENGTH + SAP_PAYLOAD_TYPE.size() + 1 + sdp.size());
    auto const length = serialize_sap_packet(header, sdp, packet);
    packet.resize(length ? *length : 0);
    return packet;
}

}  // namespace padl::sap
