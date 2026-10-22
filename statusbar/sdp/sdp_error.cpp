// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/sdp/sdp_error.hpp"

#include <string>
#include <system_error>

namespace statusbar::sdp {

auto SdpErrorCategory::message(int ev) const -> std::string
{
    switch (static_cast<SdpError>(ev)) {
        case SdpError::malformed_line:
            return "malformed line";
        case SdpError::unsupported_version:
            return "unsupported SDP version";
        case SdpError::missing_field:
            return "missing required field";
        case SdpError::unsupported_network:
            return "unsupported network or address type";
        case SdpError::unsupported_encoding:
            return "unsupported encoding";
    }
    return "unknown SDP error";
}

auto sdp_error_category() noexcept -> std::error_category const&
{
    static SdpErrorCategory const category;
    return category;
}

auto make_error_code(SdpError e) noexcept -> std::error_code
{
    return {static_cast<int>(e), sdp_error_category()};
}

}  // namespace statusbar::sdp
