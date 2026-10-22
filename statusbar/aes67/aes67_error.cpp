// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/aes67/aes67_error.hpp"

namespace statusbar::aes67 {

auto Aes67ErrorCategory::message(int const ev) const -> std::string
{
    switch (static_cast<Aes67Error>(ev)) {
        case Aes67Error::no_media:
            return "no such media in the session";
        case Aes67Error::no_connection:
            return "media has no connection address";
        case Aes67Error::not_ptp_locked:
            return "stream has no direct (PTP) media clock";
        case Aes67Error::format_mismatch:
            return "stream format differs from the slot's";
        case Aes67Error::unsupported_format:
            return "unsupported encoding or sample rate";
        case Aes67Error::packet_too_large:
            return "packet too large for a 1500-byte frame";
        case Aes67Error::unsupported_destination:
            return "destination not reachable by this transport";
        case Aes67Error::invalid_address:
            return "invalid IPv4 address";
        case Aes67Error::interface_not_found:
            return "interface not found or has no IPv4 address";
    }
    return "unknown aes67 error";
}

auto aes67_error_category() noexcept -> std::error_category const&
{
    static Aes67ErrorCategory const instance;
    return instance;
}

}  // namespace statusbar::aes67
