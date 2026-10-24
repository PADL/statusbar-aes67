#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RTP errors: RtpError and its `statusbar.rtp` category, an `is_error_code_enum`.

#include <string>
#include <system_error>
#include <type_traits>

namespace statusbar::rtp {

/// Why an RTP or RTCP writer refused a packet.
enum class RtpError
{
    no_room = 1,      ///< the caller's buffer is too small
    field_too_long,   ///< a text item, report count or packet exceeds its wire field
    unaligned_block,  ///< a profile-specific extension that is not whole 32-bit words
};

class RtpErrorCategory : public std::error_category
{
  public:
    [[nodiscard]] auto name() const noexcept -> char const* override { return "statusbar.rtp"; }
    [[nodiscard]] auto message(int ev) const -> std::string override;
};

[[nodiscard]] auto rtp_error_category() noexcept -> std::error_category const&;

[[nodiscard]] inline auto make_error_code(RtpError const e) noexcept -> std::error_code
{
    return {static_cast<int>(e), rtp_error_category()};
}

}  // namespace statusbar::rtp

template <>
struct std::is_error_code_enum<statusbar::rtp::RtpError> : std::true_type
{};
