#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include <string>
#include <system_error>
#include <type_traits>

namespace statusbar::aes67 {

enum class Aes67Error
{
    no_media = 1,             ///< the session has no media at that index
    no_connection,            ///< the media has no connection address
    not_ptp_locked,           ///< no direct media clock: the stream's timestamps are not PTP time
    format_mismatch,          ///< the stream's channels or rate is not the slot's
    unsupported_format,       ///< a format or rate AES67 or the media clock cannot carry
    packet_too_large,         ///< a packet would not fit a 1500-byte frame
    unsupported_destination,  ///< a destination this transport cannot reach
    invalid_address,          ///< not a dotted IPv4 address
    interface_not_found,      ///< no such interface, or it has no IPv4 address
    encoding_mismatch,        ///< no offered format has the slot's encoding (sample layout and width)
};

class Aes67ErrorCategory : public std::error_category
{
  public:
    [[nodiscard]] auto name() const noexcept -> char const* override { return "statusbar.aes67"; }
    [[nodiscard]] auto message(int ev) const -> std::string override;
};

[[nodiscard]] auto aes67_error_category() noexcept -> std::error_category const&;

[[nodiscard]] inline auto make_error_code(Aes67Error const e) noexcept -> std::error_code
{
    return {static_cast<int>(e), aes67_error_category()};
}

}  // namespace statusbar::aes67

template <>
struct std::is_error_code_enum<statusbar::aes67::Aes67Error> : std::true_type
{};
