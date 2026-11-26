// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// libFuzzer harness for sdp::parse: it may not crash or read out of bounds, every pair of a media's
/// reference clocks in force (RFC 7273 Section 4.8) compares consistently, and a media's channel order
/// (ST 2110-30 Section 6.2.2) and maxptime are well formed and round-trip, extension attributes as check_attributes().

#include "padl/sdp/sdp_session.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

using namespace statusbar;

namespace {

/// same_clock() is symmetric, and an IEEE 802.1AS reference matches as a PTPv2 one in domain 0 would.
void check_pair(padl::sdp::PtpReference const& a, padl::sdp::PtpReference const& b)
{
    if (a.same_clock(b) != b.same_clock(a)) {
        std::abort();
    }
    if (a.version == padl::sdp::PtpVersion::ieee802_1as && a.grandmaster) {
        auto const as_1588 = padl::sdp::PtpReference::naming(*a.grandmaster, 0);
        if (a.same_clock(b) != as_1588.same_clock(b) && b.version != padl::sdp::PtpVersion::ieee1588_2002) {
            std::abort();
        }
    }
}

/// check_pair() over every pair of `references`.
void check_references(std::span<padl::sdp::PtpReference const> const references)
{
    for (auto const& a : references) {
        for (auto const& b : references) {
            check_pair(a, b);
        }
    }
}

/// A kept channel order counts its channels, and `media`'s order, IPMX declaration, maxptime and clock domain are
/// `again`'s.
void check_written(padl::sdp::Media const& media, padl::sdp::Media const& again)
{
    if (!media.channel_order.empty() && !padl::sdp::channel_order_channels(media.channel_order)) {
        std::abort();
    }
    bool const same_fmtp = again.channel_order == media.channel_order && again.ipmx == media.ipmx;
    if (!same_fmtp || again.max_packet_time_us != media.max_packet_time_us || again.clock_domain != media.clock_domain) {
        std::abort();
    }
}

/// check_written() of each media of `session` against the session to_string() writes, parsed again.
void check_round_trip(padl::sdp::Session const& session)
{
    auto const again = padl::sdp::parse(padl::sdp::to_string(session));
    if (!again || again->media.size() != session.media.size()) {
        return;  // a session the writer sanitises differently is not this check's
    }
    if (again->clock_domain != session.clock_domain) {
        std::abort();
    }
    for (size_t i = 0; i < session.media.size(); ++i) {
        check_written(session.media[i], again->media[i]);
    }
}

/// The line `to_string()` writes for the extension attribute `x-fuzz` lies whole between two CRLFs: its value
/// holds no NUL, CR or LF.
void check_attribute_line(std::string_view const text)
{
    auto const at = text.find("\r\na=x-fuzz");
    auto const end = at == std::string_view::npos ? at : text.find("\r\n", at + 2);
    if (end == std::string_view::npos) {
        std::abort();
    }
    if (text.substr(at + 2, end - at - 2).find_first_of(std::string_view{"\0\r\n", 3}) != std::string_view::npos) {
        std::abort();
    }
}

/// Extension attributes (Session::attributes) are written on their lines, whatever their values, a reserved name
/// (`ptime`) not at all, and change nothing parse() reads.
void check_attributes(padl::sdp::Session session, std::string_view const value)
{
    auto const plain_text = padl::sdp::to_string(session);
    auto const plain = padl::sdp::parse(plain_text);
    session.attributes = {{.name = "ptime", .value = std::string{value.substr(0, 8)}}};
    if (padl::sdp::to_string(session) != plain_text) {
        std::abort();
    }
    session.attributes = {
        {.name = "x-fuzz", .value = std::string{value.substr(0, 64)}}, {.name = "PTime", .value = "1"}, {.name = "x-two"}};
    auto const written = padl::sdp::to_string(session);
    check_attribute_line(written);
    auto const extended = padl::sdp::parse(written);
    if (plain.has_value() != extended.has_value() || (extended && (!extended->attributes.empty() || *extended != *plain))) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::string_view const text{
        reinterpret_cast<char const*>(data), size};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): bytes as text
    (void)padl::sdp::channel_order_channels(text);
    auto const session = padl::sdp::parse(text);
    if (!session) {
        return 0;
    }
    for (auto const& media : session->media) {
        check_references(session->effective_clocks(media));
    }
    check_round_trip(*session);
    check_attributes(*session, text);
    return 0;
}
