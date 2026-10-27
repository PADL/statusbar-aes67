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
    BufferDeserializerBuilder reader{body};
    quadlet_t ssrc{};
    SenderInfo info{};
    size_t const blocks = size_t{count} * ReportBlock::LENGTH;
    if (!reader.parse(&ssrc).parse(&info) || body.size() - reader.position() < blocks) {
        return std::nullopt;
    }
    return RtcpSenderReport{
        .ssrc = ssrc.get(),
        .info = info,
        .blocks = {.bytes = body.subspan(reader.position(), blocks)},
        .extension = body.subspan(reader.position() + blocks),
    };
}

auto parse_receiver_report(uint8_t const count, std::span<uint8_t const> const body) noexcept -> std::optional<RtcpReceiverReport>
{
    size_t const blocks = size_t{count} * ReportBlock::LENGTH;
    if (body.size() < 4 + blocks) {
        return std::nullopt;
    }
    return RtcpReceiverReport{.ssrc = quadlet_at(body, 0), .blocks = {.bytes = body.subspan(4, blocks)}};
}

/// Takes the first CNAME and NAME from a chunk's item list; false when an item overruns it or no null octet ends it.
[[nodiscard]] auto parse_sdes_items(std::span<uint8_t const> const items, RtcpSdes& sdes) noexcept -> bool
{
    size_t at = 0;
    while (at < items.size()) {
        uint8_t const type = items[at];
        if (type == SDES_END) {
            return true;
        }
        if (items.size() - at < 2 || items.size() - at - 2 < items[at + 1]) {
            return false;
        }
        auto const text = as_string_view(items.subspan(at + 2, items[at + 1]));
        if (type == SDES_CNAME && sdes.cname.empty()) {
            sdes.cname = text;
        } else if (type == SDES_NAME && sdes.name.empty()) {
            sdes.name = text;
        }
        at += 2 + size_t{items[at + 1]};
    }
    return false;  // no terminating null octet
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
    if (!parse_sdes_items(body.subspan(4), sdes)) {
        return std::nullopt;
    }
    return sdes;
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

/// A sub-packet of a compound packet: its header, all of its bytes, and its body without padding.
struct RtcpSubpacket
{
    RtcpHeader header{};
    std::span<uint8_t const> packet;
    std::span<uint8_t const> body;
};

/// The header at the start of `remaining`; std::nullopt when it is not version 2, overruns `remaining`,
/// or, being the `first`, is not an SR or RR.
[[nodiscard]] auto parse_rtcp_header(std::span<uint8_t const> const remaining, bool const first) noexcept
    -> std::optional<RtcpHeader>
{
    RtcpHeader header{};
    BufferDeserializerBuilder reader{remaining};
    reader.parse(&header);
    uint8_t const type = header.packet_type.get();
    if (!reader || header.version() != RtcpHeader::VERSION || header.size() > remaining.size() ||
        (first && type != RTCP_SR && type != RTCP_RR)) {
        return std::nullopt;
    }
    return header;
}

/// The padding octets ending `packet`; std::nullopt when padding is on a packet other than the `last`, on the `first`,
/// or its count is out of range.
[[nodiscard]] auto padding_length(
    RtcpHeader const& header, std::span<uint8_t const> const packet, bool const first, bool const last) noexcept
    -> std::optional<size_t>
{
    if (!header.padding()) {
        return 0;
    }
    size_t const pad = packet.back();  // counts itself
    if (!last || first || pad == 0 || pad > packet.size() - RtcpHeader::LENGTH) {
        return std::nullopt;
    }
    return pad;
}

/// The sub-packet at the start of `remaining`; std::nullopt when malformed (RFC 3550 Appendix A.2).
[[nodiscard]] auto parse_subpacket(std::span<uint8_t const> const remaining, bool const first) noexcept
    -> std::optional<RtcpSubpacket>
{
    auto const header = parse_rtcp_header(remaining, first);
    if (!header) {
        return std::nullopt;
    }
    auto const packet = remaining.first(header->size());
    auto const pad = padding_length(*header, packet, first, packet.size() == remaining.size());
    if (!pad) {
        return std::nullopt;
    }
    return RtcpSubpacket{
        .header = *header, .packet = packet, .body = packet.subspan(RtcpHeader::LENGTH, packet.size() - RtcpHeader::LENGTH - *pad)};
}

/// Keeps the first of each kind of sub-packet in `slot`; false when this one did not parse.
template <typename T>
[[nodiscard]] auto keep_first(std::optional<T> const& parsed, std::optional<T>& slot) noexcept -> bool
{
    if (!parsed) {
        return false;
    }
    if (!slot) {
        slot = *parsed;
    }
    return true;
}

/// Adds a sub-packet to the compound; false when its body is malformed. APP, XR and the rest are skipped.
[[nodiscard]] auto add_subpacket(RtcpSubpacket const& sub, RtcpCompound& compound) noexcept -> bool
{
    uint8_t const count = sub.header.count();
    switch (sub.header.packet_type.get()) {
        case RTCP_SR:
            return keep_first(parse_sender_report(count, sub.body), compound.sr);
        case RTCP_RR:
            return keep_first(parse_receiver_report(count, sub.body), compound.rr);
        case RTCP_SDES:
            return count == 0 || keep_first(parse_sdes(count, sub.body), compound.sdes);  // an empty SDES is not kept
        case RTCP_BYE:
            return keep_first(parse_bye(count, sub.body), compound.bye);
        default:
            return true;
    }
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
        auto const sub = parse_subpacket(datagram.subspan(offset), compound.packets == 0);
        if (!sub || !add_subpacket(*sub, compound)) {
            return std::nullopt;
        }
        ++compound.packets;
        offset += sub->packet.size();
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

void RtcpWriter::refuse_if(bool const condition, RtpError const error) noexcept
{
    if (!error_ && condition) {
        error_ = make_error_code(error);
    }
}

auto RtcpWriter::begin_packet(uint8_t const count, uint8_t const type, size_t const size, uint32_t const ssrc) noexcept -> bool
{
    if (!reserve(size)) {
        return false;
    }
    RtcpHeader header{};
    header.init(count, type, size);
    writer_.append(header).append(quadlet_t{ssrc});
    return true;
}

void RtcpWriter::append_blocks(std::span<ReportBlock const> const blocks) noexcept
{
    for (auto const& block : blocks) {
        writer_.append(block);
    }
}

void RtcpWriter::append_sdes_item(uint8_t const type, std::string_view const text) noexcept
{
    writer_.append(octet_t{type}).append(octet_t{static_cast<uint8_t>(text.size())}).append(make_const_span(text));
}

auto RtcpWriter::sender_report(
    uint32_t const ssrc,
    SenderInfo const& info,
    std::span<ReportBlock const> const blocks,
    std::span<uint8_t const> const extension) noexcept -> RtcpWriter&
{
    refuse_if(blocks.size() > RtcpHeader::MAX_COUNT, RtpError::field_too_long);
    refuse_if(extension.size() % 4 != 0, RtpError::unaligned_block);
    size_t const size = RtcpHeader::LENGTH + 4 + SenderInfo::LENGTH + (blocks.size() * ReportBlock::LENGTH) + extension.size();
    if (begin_packet(static_cast<uint8_t>(blocks.size()), RTCP_SR, size, ssrc)) {
        writer_.append(info);
        append_blocks(blocks);
        writer_.append(extension);
    }
    return *this;
}

auto RtcpWriter::receiver_report(uint32_t const ssrc, std::span<ReportBlock const> const blocks) noexcept -> RtcpWriter&
{
    refuse_if(blocks.size() > RtcpHeader::MAX_COUNT, RtpError::field_too_long);
    size_t const size = RtcpHeader::LENGTH + 4 + (blocks.size() * ReportBlock::LENGTH);
    if (begin_packet(static_cast<uint8_t>(blocks.size()), RTCP_RR, size, ssrc)) {
        append_blocks(blocks);
    }
    return *this;
}

auto RtcpWriter::sdes(uint32_t const ssrc, std::string_view const cname, std::string_view const name) noexcept -> RtcpWriter&
{
    refuse_if(cname.size() > 255 || name.size() > 255, RtpError::field_too_long);
    // The items, then at least one null octet ending the list and padding the chunk.
    size_t const items = 2 + cname.size() + (name.empty() ? 0 : 2 + name.size());
    size_t const chunk = padded_to_word(4 + items + 1);
    if (begin_packet(1, RTCP_SDES, RtcpHeader::LENGTH + chunk, ssrc)) {
        append_sdes_item(SDES_CNAME, cname);
        if (!name.empty()) {
            append_sdes_item(SDES_NAME, name);
        }
        append_zeros(chunk - 4 - items);
    }
    return *this;
}

auto RtcpWriter::bye(uint32_t const ssrc, std::string_view const reason) noexcept -> RtcpWriter&
{
    refuse_if(reason.size() > 255, RtpError::field_too_long);
    size_t const tail = reason.empty() ? 0 : padded_to_word(1 + reason.size());
    if (begin_packet(1, RTCP_BYE, RtcpHeader::LENGTH + 4 + tail, ssrc) && !reason.empty()) {
        writer_.append(octet_t{static_cast<uint8_t>(reason.size())}).append(make_const_span(reason));
        append_zeros(tail - 1 - reason.size());
    }
    return *this;
}

}  // namespace statusbar::rtp
