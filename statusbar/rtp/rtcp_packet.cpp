// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtcp_packet.hpp"

#include "statusbar/rtp/rtp_error.hpp"

#include <array>

namespace statusbar::rtp {

namespace {

constexpr std::array<uint8_t, 4> ZEROS{};

auto quadlet_at(std::span<uint8_t const> const bytes, size_t const offset) noexcept -> uint32_t
{
    BufferDeserializerBuilder reader{bytes.subspan(offset, 4)};
    quadlet_t value{};
    reader.parse(&value);
    return value.get();
}

auto parse_sender_report(uint8_t const count, std::span<uint8_t const> const body) noexcept -> std::optional<RtcpSenderReport>
{
    RtcpSenderReport sr{};
    BufferDeserializerBuilder reader{body};
    quadlet_t ssrc{};
    reader.parse(&ssrc).parse(&sr.info);
    size_t const blocks = size_t{count} * ReportBlock::LENGTH;
    if (!reader || body.size() - reader.position() < blocks) {
        return std::nullopt;
    }
    sr.ssrc = ssrc.get();
    sr.blocks.bytes = body.subspan(reader.position(), blocks);
    sr.extension = body.subspan(reader.position() + blocks);
    return sr;
}

auto parse_receiver_report(uint8_t const count, std::span<uint8_t const> const body) noexcept -> std::optional<RtcpReceiverReport>
{
    size_t const blocks = size_t{count} * ReportBlock::LENGTH;
    if (body.size() < 4 + blocks) {
        return std::nullopt;
    }
    return RtcpReceiverReport{.ssrc = quadlet_at(body, 0), .blocks = {.bytes = body.subspan(4, blocks)}};
}

/// The first chunk; the rest are not looked at. Its item list must end with a null octet (Section 6.5).
auto parse_sdes(uint8_t const count, std::span<uint8_t const> const body) noexcept -> std::optional<RtcpSdes>
{
    RtcpSdes sdes{};
    if (count == 0) {
        return sdes;
    }
    if (body.size() < 4) {
        return std::nullopt;
    }
    sdes.ssrc = quadlet_at(body, 0);
    size_t at = 4;
    while (at < body.size()) {
        uint8_t const type = body[at];
        if (type == SDES_END) {
            return sdes;
        }
        if (body.size() - at < 2 || body.size() - at - 2 < body[at + 1]) {
            return std::nullopt;
        }
        auto const text = as_string_view(body.subspan(at + 2, body[at + 1]));
        if (type == SDES_CNAME && sdes.cname.empty()) {
            sdes.cname = text;
        } else if (type == SDES_NAME && sdes.name.empty()) {
            sdes.name = text;
        }
        at += 2 + size_t{body[at + 1]};
    }
    return std::nullopt;  // no terminating null octet
}

auto parse_bye(uint8_t const count, std::span<uint8_t const> const body) noexcept -> std::optional<RtcpBye>
{
    size_t const sources = size_t{count} * 4;
    if (body.size() < sources) {
        return std::nullopt;
    }
    RtcpBye bye{.ssrc_bytes = body.first(sources), .reason = {}};
    auto const rest = body.subspan(sources);
    if (!rest.empty()) {
        if (rest.size() - 1 < rest[0]) {
            return std::nullopt;
        }
        bye.reason = as_string_view(rest.subspan(1, rest[0]));
    }
    return bye;
}

}  // namespace

auto RtcpReportBlocks::at(size_t const index) const noexcept -> ReportBlock
{
    ReportBlock block{};
    BufferDeserializerBuilder reader{bytes.subspan(index * ReportBlock::LENGTH, ReportBlock::LENGTH)};
    reader.parse(&block);
    return block;
}

auto RtcpReportBlocks::find(uint32_t const ssrc) const noexcept -> std::optional<ReportBlock>
{
    for (size_t i = 0; i < count(); ++i) {
        if (auto const block = at(i); block.ssrc.get() == ssrc) {
            return block;
        }
    }
    return std::nullopt;
}

auto RtcpBye::contains(uint32_t const ssrc) const noexcept -> bool
{
    for (size_t i = 0; i < count(); ++i) {
        if (quadlet_at(ssrc_bytes, i * 4) == ssrc) {
            return true;
        }
    }
    return false;
}

auto parse_rtcp(std::span<uint8_t const> const datagram) noexcept -> std::optional<RtcpCompound>
{
    if (datagram.size() < RtcpHeader::LENGTH || datagram.size() % 4 != 0) {
        return std::nullopt;
    }
    RtcpCompound compound{};
    size_t offset = 0;
    while (offset < datagram.size()) {
        auto const remaining = datagram.subspan(offset);
        RtcpHeader header{};
        BufferDeserializerBuilder reader{remaining};
        reader.parse(&header);
        if (!reader || header.version() != RtcpHeader::VERSION || header.size() > remaining.size()) {
            return std::nullopt;
        }
        auto const packet = remaining.first(header.size());
        bool const last = header.size() == remaining.size();
        size_t pad = 0;
        if (header.padding()) {
            pad = packet.back();  // counts itself
            if (!last || compound.packets == 0 || pad == 0 || pad > packet.size() - RtcpHeader::LENGTH) {
                return std::nullopt;
            }
        }
        auto const body = packet.subspan(RtcpHeader::LENGTH, packet.size() - RtcpHeader::LENGTH - pad);
        uint8_t const type = header.packet_type.get();
        if (compound.packets == 0 && type != RTCP_SR && type != RTCP_RR) {
            return std::nullopt;
        }
        switch (type) {
            case RTCP_SR: {
                auto sr = parse_sender_report(header.count(), body);
                if (!sr) {
                    return std::nullopt;
                }
                if (!compound.sr) {
                    compound.sr = *sr;
                }
                break;
            }
            case RTCP_RR: {
                auto rr = parse_receiver_report(header.count(), body);
                if (!rr) {
                    return std::nullopt;
                }
                if (!compound.rr) {
                    compound.rr = *rr;
                }
                break;
            }
            case RTCP_SDES: {
                auto sdes = parse_sdes(header.count(), body);
                if (!sdes) {
                    return std::nullopt;
                }
                if (!compound.sdes && header.count() != 0) {
                    compound.sdes = *sdes;
                }
                break;
            }
            case RTCP_BYE: {
                auto bye = parse_bye(header.count(), body);
                if (!bye) {
                    return std::nullopt;
                }
                if (!compound.bye) {
                    compound.bye = *bye;
                }
                break;
            }
            default:
                break;  // APP, XR and the rest: skipped
        }
        ++compound.packets;
        offset += packet.size();
    }
    return compound;
}

auto RtcpWriter::reserve(size_t const size) noexcept -> bool
{
    if (error_) {
        return false;
    }
    if (size > (size_t{0xFFFF} + 1) * 4) {
        error_ = make_error_code(RtpError::field_too_long);
        return false;
    }
    if (writer_.buffer().available_space() < size) {
        error_ = make_error_code(RtpError::no_room);
        return false;
    }
    return true;
}

void RtcpWriter::append_zeros(size_t count) noexcept
{
    while (count > 0) {
        size_t const n = count < ZEROS.size() ? count : ZEROS.size();
        writer_.append(std::span<uint8_t const>{ZEROS}.first(n));
        count -= n;
    }
}

auto RtcpWriter::sender_report(
    uint32_t const ssrc,
    SenderInfo const& info,
    std::span<ReportBlock const> const blocks,
    std::span<uint8_t const> const extension) noexcept -> RtcpWriter&
{
    if (!error_ && blocks.size() > RtcpHeader::MAX_COUNT) {
        error_ = make_error_code(RtpError::field_too_long);
    }
    if (!error_ && extension.size() % 4 != 0) {
        error_ = make_error_code(RtpError::unaligned_block);
    }
    size_t const size = RtcpHeader::LENGTH + 4 + SenderInfo::LENGTH + (blocks.size() * ReportBlock::LENGTH) + extension.size();
    if (!reserve(size)) {
        return *this;
    }
    RtcpHeader header{};
    header.init(static_cast<uint8_t>(blocks.size()), RTCP_SR, size);
    writer_.append(header).append(quadlet_t{ssrc}).append(info);
    for (auto const& block : blocks) {
        writer_.append(block);
    }
    writer_.append(extension);
    return *this;
}

auto RtcpWriter::receiver_report(uint32_t const ssrc, std::span<ReportBlock const> const blocks) noexcept -> RtcpWriter&
{
    if (!error_ && blocks.size() > RtcpHeader::MAX_COUNT) {
        error_ = make_error_code(RtpError::field_too_long);
    }
    size_t const size = RtcpHeader::LENGTH + 4 + (blocks.size() * ReportBlock::LENGTH);
    if (!reserve(size)) {
        return *this;
    }
    RtcpHeader header{};
    header.init(static_cast<uint8_t>(blocks.size()), RTCP_RR, size);
    writer_.append(header).append(quadlet_t{ssrc});
    for (auto const& block : blocks) {
        writer_.append(block);
    }
    return *this;
}

auto RtcpWriter::sdes(uint32_t const ssrc, std::string_view const cname, std::string_view const name) noexcept -> RtcpWriter&
{
    if (!error_ && (cname.size() > 255 || name.size() > 255)) {
        error_ = make_error_code(RtpError::field_too_long);
    }
    // The items, then at least one null octet ending the list and padding the chunk.
    size_t const items = 2 + cname.size() + (name.empty() ? 0 : 2 + name.size());
    size_t const chunk = padded_to_word(4 + items + 1);
    size_t const size = RtcpHeader::LENGTH + chunk;
    if (!reserve(size)) {
        return *this;
    }
    RtcpHeader header{};
    header.init(1, RTCP_SDES, size);
    writer_.append(header).append(quadlet_t{ssrc});
    writer_.append(octet_t{SDES_CNAME}).append(octet_t{static_cast<uint8_t>(cname.size())}).append(make_const_span(cname));
    if (!name.empty()) {
        writer_.append(octet_t{SDES_NAME}).append(octet_t{static_cast<uint8_t>(name.size())}).append(make_const_span(name));
    }
    append_zeros(chunk - 4 - items);
    return *this;
}

auto RtcpWriter::bye(uint32_t const ssrc, std::string_view const reason) noexcept -> RtcpWriter&
{
    if (!error_ && reason.size() > 255) {
        error_ = make_error_code(RtpError::field_too_long);
    }
    size_t const tail = reason.empty() ? 0 : padded_to_word(1 + reason.size());
    size_t const size = RtcpHeader::LENGTH + 4 + tail;
    if (!reserve(size)) {
        return *this;
    }
    RtcpHeader header{};
    header.init(1, RTCP_BYE, size);
    writer_.append(header).append(quadlet_t{ssrc});
    if (!reason.empty()) {
        writer_.append(octet_t{static_cast<uint8_t>(reason.size())}).append(make_const_span(reason));
        append_zeros(tail - 1 - reason.size());
    }
    return *this;
}

}  // namespace statusbar::rtp
