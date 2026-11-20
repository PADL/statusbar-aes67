#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// Text helpers sdp_parser.cpp and sdp_writer.cpp share; internal to the module, so not in sdp.hpp.

#include <algorithm>
#include <cctype>
#include <string_view>

namespace padl::sdp::detail {

/// Whether `a` and `b` are the same text but for the case of their ASCII letters.
[[nodiscard]] inline auto equal_ignoring_case(std::string_view const a, std::string_view const b) noexcept -> bool
{
    return std::ranges::equal(a, b, [](char const x, char const y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

}  // namespace padl::sdp::detail
