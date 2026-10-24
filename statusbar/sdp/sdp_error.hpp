#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// SDP errors: SdpError and its `statusbar.sdp` category, an `is_error_code_enum`.

#include <string>
#include <system_error>
#include <type_traits>

namespace statusbar::sdp {

/// SDP parsing error codes
enum class SdpError
{
    malformed_line = 1,    ///< not "<type>=<value>", or a field that does not parse
    unsupported_version,   ///< v= other than 0
    missing_field,         ///< a required line or attribute is absent
    unsupported_network,   ///< a network or address type other than IN IP4
    unsupported_encoding,  ///< an rtpmap other than L16, L24, L32 or AM824
};

/// Error category for SDP errors
class SdpErrorCategory : public std::error_category
{
  public:
    [[nodiscard]] auto name() const noexcept -> char const* override { return "statusbar.sdp"; }

    [[nodiscard]] auto message(int ev) const -> std::string override;
};

/// Get the SDP error category singleton
[[nodiscard]] auto sdp_error_category() noexcept -> std::error_category const&;

/// Create an error_code from an SdpError
[[nodiscard]] auto make_error_code(SdpError e) noexcept -> std::error_code;

}  // namespace statusbar::sdp

template <>
struct std::is_error_code_enum<statusbar::sdp::SdpError> : std::true_type
{};
