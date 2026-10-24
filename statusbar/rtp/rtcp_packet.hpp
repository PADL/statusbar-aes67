#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RTCP - the RFC 3550 Section 6 packets an AES67 or IPMX stream needs (SR, RR, SDES, BYE) as wire
/// structs, a compound-packet parser that views the datagram, and a writer into a caller's buffer.

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace statusbar::rtp {

using ieee::doublet_t;
using ieee::octet_t;
using ieee::quadlet_t;

/// `n` rounded up to whole 32-bit words, as RTCP pads its items and packets.
[[nodiscard]] constexpr auto padded_to_word(size_t const n) noexcept -> size_t
{
    return (n + 3U) & ~size_t{3};
}

//
// Packet types - RFC 3550 Section 12.1
//

inline constexpr uint8_t RTCP_SR = 200;
inline constexpr uint8_t RTCP_RR = 201;
inline constexpr uint8_t RTCP_SDES = 202;
inline constexpr uint8_t RTCP_BYE = 203;
inline constexpr uint8_t RTCP_APP = 204;

//
// SDES item types - RFC 3550 Section 6.5
//

inline constexpr uint8_t SDES_END = 0;
inline constexpr uint8_t SDES_CNAME = 1;
inline constexpr uint8_t SDES_NAME = 2;

/// The common header of every RTCP packet - RFC 3550 Section 6.4.1 (4 bytes)
struct RtcpHeader
{
    static constexpr size_t LENGTH = 4;
    static constexpr uint8_t VERSION = 2;
    /// Report blocks, SDES chunks or BYE sources one packet holds at most (a 5-bit count)
    static constexpr uint8_t MAX_COUNT = 31;

    /// Byte 0: version[7:6] | padding[5] | count[4:0] (RC, SC or the BYE source count)
    octet_t version_p_count;
    octet_t packet_type;
    /// The packet's length in 32-bit words minus one, header and padding included
    doublet_t length;

    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return version_p_count.get_bits(0xC0U, 6); }
    [[nodiscard]] constexpr auto padding() const noexcept -> bool { return version_p_count.has_flag(0x20U); }
    [[nodiscard]] constexpr auto count() const noexcept -> uint8_t { return version_p_count.get_bits(0x1FU, 0); }
    /// The packet's length in bytes, header included.
    [[nodiscard]] constexpr auto size() const noexcept -> size_t { return (size_t{length.get()} + 1) * 4; }

    /// A version 2 header without padding; `bytes` is the packet's whole length, a multiple of 4.
    constexpr void init(uint8_t const count, uint8_t const type, size_t const bytes) noexcept
    {
        version_p_count = static_cast<uint8_t>((VERSION << 6U) | (count & 0x1FU));
        packet_type = type;
        length = static_cast<uint16_t>((bytes / 4) - 1);
    }
};

static_assert(sizeof(RtcpHeader) == RtcpHeader::LENGTH);
static_assert(alignof(RtcpHeader) == 1);
static_assert(offsetof(RtcpHeader, length) == 2);

/// An SR's sender information - RFC 3550 Section 6.4.1 (20 bytes). The 64-bit time field is
/// whatever the sender put there: an IPMX sender's PTP truncated time (TR-10-1 Section 8.7).
struct SenderInfo
{
    static constexpr size_t LENGTH = 20;

    quadlet_t ntp_msw;
    quadlet_t ntp_lsw;
    quadlet_t rtp_timestamp;
    quadlet_t packet_count;
    quadlet_t octet_count;  ///< payload octets, headers and padding excluded
};

static_assert(sizeof(SenderInfo) == SenderInfo::LENGTH);
static_assert(alignof(SenderInfo) == 1);
static_assert(offsetof(SenderInfo, rtp_timestamp) == 8);
static_assert(offsetof(SenderInfo, octet_count) == 16);

/// One source's reception report - RFC 3550 Section 6.4.1 (24 bytes)
struct ReportBlock
{
    static constexpr size_t LENGTH = 24;

    quadlet_t ssrc;
    /// fraction lost[31:24] (1/256ths) | cumulative lost[23:0] (signed)
    quadlet_t loss;
    quadlet_t extended_highest_seq;
    quadlet_t jitter;  ///< interarrival jitter, timestamp units
    quadlet_t lsr;     ///< middle 32 bits of the last SR's time field; 0 before one
    quadlet_t dlsr;    ///< since that SR arrived, 1/65536 s; 0 before one

    [[nodiscard]] constexpr auto fraction_lost() const noexcept -> uint8_t { return static_cast<uint8_t>(loss.get() >> 24U); }
    /// Sign-extended from 24 bits.
    [[nodiscard]] constexpr auto cumulative_lost() const noexcept -> int32_t
    {
        uint32_t const raw = loss.get() & 0x00FF'FFFFU;
        return (raw & 0x0080'0000U) != 0 ? static_cast<int32_t>(raw | 0xFF00'0000U) : static_cast<int32_t>(raw);
    }
    /// `cumulative` is clamped to the 24-bit signed field.
    constexpr void set_loss(uint8_t const fraction, int32_t const cumulative) noexcept
    {
        int32_t const clamped = cumulative < -0x80'0000 ? -0x80'0000 : (cumulative > 0x7F'FFFF ? 0x7F'FFFF : cumulative);
        loss = (uint32_t{fraction} << 24U) | (static_cast<uint32_t>(clamped) & 0x00FF'FFFFU);
    }
};

static_assert(sizeof(ReportBlock) == ReportBlock::LENGTH);
static_assert(alignof(ReportBlock) == 1);
static_assert(offsetof(ReportBlock, extended_highest_seq) == 8);
static_assert(offsetof(ReportBlock, dlsr) == 20);

/// A packet's report blocks, viewed in the datagram.
struct RtcpReportBlocks
{
    std::span<uint8_t const> bytes;  ///< count() * ReportBlock::LENGTH

    [[nodiscard]] auto count() const noexcept -> size_t { return bytes.size() / ReportBlock::LENGTH; }
    /// Block `index` (< count()).
    [[nodiscard]] auto at(size_t index) const noexcept -> ReportBlock;
    /// The block about `ssrc`, or nullopt.
    [[nodiscard]] auto find(uint32_t ssrc) const noexcept -> std::optional<ReportBlock>;
};

/// A parsed SR. `extension` is the profile-specific extension (RFC 3550 Section 6.4.3) after the
/// report blocks, padding excluded: an IPMX sender's Info Block (rtcp_ipmx.hpp).
struct RtcpSenderReport
{
    uint32_t ssrc{0};
    SenderInfo info{};
    RtcpReportBlocks blocks;
    std::span<uint8_t const> extension;
};

/// A parsed RR (any profile-specific extension is ignored).
struct RtcpReceiverReport
{
    uint32_t ssrc{0};
    RtcpReportBlocks blocks;
};

/// An SDES packet's first chunk: its source's CNAME and NAME (empty when absent).
struct RtcpSdes
{
    uint32_t ssrc{0};
    std::string_view cname;
    std::string_view name;
};

/// A parsed BYE: the sources leaving, viewed in the datagram, and the reason (may be empty).
struct RtcpBye
{
    std::span<uint8_t const> ssrc_bytes;  ///< 4 bytes per source
    std::string_view reason;

    [[nodiscard]] auto count() const noexcept -> size_t { return ssrc_bytes.size() / 4; }
    [[nodiscard]] auto contains(uint32_t ssrc) const noexcept -> bool;
};

/// What a compound packet held: the first of each kind, views into the datagram, valid while it is.
struct RtcpCompound
{
    std::optional<RtcpSenderReport> sr;
    std::optional<RtcpReceiverReport> rr;
    std::optional<RtcpSdes> sdes;
    std::optional<RtcpBye> bye;
    size_t packets{0};  ///< sub-packets walked, unknown types included
};

/// Walk a compound packet with RFC 3550 Appendix A.2's checks: every sub-packet version 2, the
/// first an SR or RR without padding, padding only on the last, the lengths summing to the datagram. Unknown types
/// (APP, XR) are skipped, as TR-10-1 Section 8.7 asks. Nullopt when malformed.
[[nodiscard]] auto parse_rtcp(std::span<uint8_t const> datagram) noexcept -> std::optional<RtcpCompound>;

/// Appends RTCP packets to a caller's buffer, forming a compound packet (RFC 3550 Section 6.1:
/// a report first, then SDES with the CNAME). Sticky-error: after a refusal nothing more is written.
class RtcpWriter
{
  public:
    explicit RtcpWriter(std::span<uint8_t> out) noexcept
        : writer_{out}
    {}
    RtcpWriter(RtcpWriter const&) = delete;
    auto operator=(RtcpWriter const&) -> RtcpWriter& = delete;
    RtcpWriter(RtcpWriter&&) = delete;
    auto operator=(RtcpWriter&&) -> RtcpWriter& = delete;
    ~RtcpWriter() = default;

    /// An SR; `extension` (a multiple of 4 bytes) follows the blocks and is counted in its length.
    auto sender_report(
        uint32_t ssrc,
        SenderInfo const& info,
        std::span<ReportBlock const> blocks = {},
        std::span<uint8_t const> extension = {}) noexcept -> RtcpWriter&;
    auto receiver_report(uint32_t ssrc, std::span<ReportBlock const> blocks = {}) noexcept -> RtcpWriter&;
    /// One chunk: CNAME, then NAME when not empty (each at most 255 bytes).
    auto sdes(uint32_t ssrc, std::string_view cname, std::string_view name = {}) noexcept -> RtcpWriter&;
    /// One source leaving, with an optional reason (at most 255 bytes).
    auto bye(uint32_t ssrc, std::string_view reason = {}) noexcept -> RtcpWriter&;

    /// No refusal so far.
    [[nodiscard]] explicit operator bool() const noexcept { return !error_; }
    /// The first refusal, an RtpError.
    [[nodiscard]] auto status() const noexcept -> Status { return error_ ? failure(error_) : success(); }
    /// The bytes written so far.
    [[nodiscard]] auto bytes() const noexcept -> std::span<uint8_t const> { return writer_.get_span(); }

  private:
    /// Room for `size` more bytes, else the sticky error.
    auto reserve(size_t size) noexcept -> bool;
    void append_zeros(size_t count) noexcept;

    BufferSerializerBuilderWithBuffer writer_;
    std::error_code error_;
};

}  // namespace statusbar::rtp

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::rtp::RtcpHeader> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::rtp::SenderInfo> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::rtp::ReportBlock> : std::true_type
{};

namespace statusbar::rtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::rtp
