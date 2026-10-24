// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtp_error.hpp"

namespace statusbar::rtp {

auto RtpErrorCategory::message(int const ev) const -> std::string
{
    switch (static_cast<RtpError>(ev)) {
        case RtpError::no_room:
            return "packet does not fit the buffer";
        case RtpError::field_too_long:
            return "item too long for its wire field";
        case RtpError::unaligned_block:
            return "extension is not whole 32-bit words";
    }
    return "unknown rtp error";
}

auto rtp_error_category() noexcept -> std::error_category const&
{
    static RtpErrorCategory const instance;
    return instance;
}

}  // namespace statusbar::rtp
