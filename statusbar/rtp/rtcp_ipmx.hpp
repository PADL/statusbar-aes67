#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// The IPMX Info Block an IPMX SR carries as its profile-specific extension (VSF TR-10-1 Section
/// 8.7), with the PCM digital audio Media Info Block (VSF TR-10-3 Section 11). Its presence says the
/// SR's time field is PTP truncated time.

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace statusbar::rtp {

inline constexpr uint16_t IPMX_TAG = 0x5831;  ///< "X1"
inline constexpr uint16_t IPMX_MEDIA_PCM = 0x0002;

/// The IPMX Info Block's fixed part: tag, length, version, the two strings - TR-10-1 Figure 1 (84 bytes)
struct IpmxInfoBlockHeader
{
    static constexpr size_t LENGTH = 84;
    static constexpr size_t TS_REFCLK_LENGTH = 64;
    static constexpr size_t MEDIACLK_LENGTH = 12;

    ieee::doublet_t tag;
    ieee::doublet_t length;  ///< 32-bit words of the whole Info Block, Media Info Blocks included, minus one
    ieee::octet_t version;
    std::array<uint8_t, 3> reserved;
    std::array<uint8_t, TS_REFCLK_LENGTH> ts_refclk;  ///< NUL padded
    std::array<uint8_t, MEDIACLK_LENGTH> mediaclk;    ///< NUL padded
};

static_assert(sizeof(IpmxInfoBlockHeader) == IpmxInfoBlockHeader::LENGTH);
static_assert(alignof(IpmxInfoBlockHeader) == 1);
static_assert(offsetof(IpmxInfoBlockHeader, ts_refclk) == 8);
static_assert(offsetof(IpmxInfoBlockHeader, mediaclk) == 72);

/// The PCM Media Info Block's fixed part - TR-10-3 Figure 1 (20 bytes), then the channel-order string
struct IpmxPcmMediaInfoHeader
{
    static constexpr size_t LENGTH = 20;

    ieee::doublet_t type;
    ieee::doublet_t length;  ///< 32-bit words of this block minus one
    ieee::quadlet_t sampling_rate;
    ieee::octet_t sample_size;
    ieee::octet_t channel_count;
    ieee::doublet_t packet_time;  ///< microseconds
    ieee::quadlet_t measured_sample_rate;
    ieee::quadlet_t channel_order_words;  ///< the string's length in 32-bit words, padding included
};

static_assert(sizeof(IpmxPcmMediaInfoHeader) == IpmxPcmMediaInfoHeader::LENGTH);
static_assert(alignof(IpmxPcmMediaInfoHeader) == 1);
static_assert(offsetof(IpmxPcmMediaInfoHeader, packet_time) == 10);
static_assert(offsetof(IpmxPcmMediaInfoHeader, channel_order_words) == 16);

/// A PCM stream as its Media Info Block describes it.
struct IpmxPcmMediaInfo
{
    uint32_t sampling_rate{0};  ///< Hz, as the rtpmap
    uint8_t sample_size{0};     ///< bits: 16 or 24
    uint8_t channel_count{0};
    uint16_t packet_time_us{0};        ///< the ptime in microseconds
    uint32_t measured_sample_rate{0};  ///< Hz (TR-10-1 Section 10.3); the nominal rate when PTP locked
    std::string_view channel_order;    ///< ST 2110-30 Section 6.2.2, e.g. "SMPTE2110.(U08)"

    auto operator==(IpmxPcmMediaInfo const&) const noexcept -> bool = default;
};

/// An IPMX Info Block. Parsed, its strings view the extension's bytes.
struct IpmxInfoBlock
{
    uint8_t version{0};          ///< incremented on any change of the content
    std::string_view ts_refclk;  ///< after "a=ts-refclk:", at most 64 bytes
    std::string_view mediaclk;   ///< after "a=mediaclk:", at most 12 bytes
    std::optional<IpmxPcmMediaInfo> pcm;

    auto operator==(IpmxInfoBlock const&) const noexcept -> bool = default;
};

/// The bytes serialize_ipmx_info_block() writes for `block`.
[[nodiscard]] auto ipmx_info_block_size(IpmxInfoBlock const& block) noexcept -> size_t;

/// Write `block` into `out`: its length, or nullopt when a string is too long or `out` too small.
[[nodiscard]] auto serialize_ipmx_info_block(IpmxInfoBlock const& block, std::span<uint8_t> out) noexcept -> std::optional<size_t>;

/// Parse an SR's extension: nullopt when it is not an IPMX Info Block (another tag) or is malformed.
/// Unknown Media Info Blocks are skipped by their length; the first PCM one is kept.
[[nodiscard]] auto parse_ipmx_info_block(std::span<uint8_t const> extension) noexcept -> std::optional<IpmxInfoBlock>;

}  // namespace statusbar::rtp

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::rtp::IpmxInfoBlockHeader> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::rtp::IpmxPcmMediaInfoHeader> : std::true_type
{};

namespace statusbar::rtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::rtp
