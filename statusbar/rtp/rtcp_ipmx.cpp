// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/rtp/rtcp_ipmx.hpp"

#include "statusbar/rtp/rtcp_packet.hpp"

#include <algorithm>
#include <array>

namespace statusbar::rtp {

namespace {

/// The text of a NUL-padded field: up to its first NUL.
auto unpad(std::span<uint8_t const> const field) noexcept -> std::string_view
{
    auto const end = std::ranges::find(field, uint8_t{0});
    return as_string_view(field.first(static_cast<size_t>(end - field.begin())));
}

/// `text` in a NUL-padded field of N bytes; the caller has checked that it fits.
template <size_t N>
[[nodiscard]] auto padded(std::string_view const text) noexcept -> std::array<uint8_t, N>
{
    std::array<uint8_t, N> field{};
    std::ranges::copy(text, field.begin());
    return field;
}

/// The Info Block's fixed part for a block of `size` bytes (TR-10-1 Figure 1).
[[nodiscard]] auto info_block_header(IpmxInfoBlock const& block, size_t const size) noexcept -> IpmxInfoBlockHeader
{
    return IpmxInfoBlockHeader{
        .tag = IPMX_TAG,
        .length = static_cast<uint16_t>((size / 4) - 1),
        .version = block.version,
        .reserved = {},
        .ts_refclk = padded<IpmxInfoBlockHeader::TS_REFCLK_LENGTH>(block.ts_refclk),
        .mediaclk = padded<IpmxInfoBlockHeader::MEDIACLK_LENGTH>(block.mediaclk),
    };
}

/// The PCM Media Info Block's fixed part, its channel-order string taking `order` bytes (TR-10-3 Figure 1).
[[nodiscard]] auto pcm_media_header(IpmxPcmMediaInfo const& pcm, size_t const order) noexcept -> IpmxPcmMediaInfoHeader
{
    return IpmxPcmMediaInfoHeader{
        .type = IPMX_MEDIA_PCM,
        .length = static_cast<uint16_t>(((IpmxPcmMediaInfoHeader::LENGTH + order) / 4) - 1),
        .sampling_rate = pcm.sampling_rate,
        .sample_size = pcm.sample_size,
        .channel_count = pcm.channel_count,
        .packet_time = pcm.packet_time_us,
        .measured_sample_rate = pcm.measured_sample_rate,
        .channel_order_words = static_cast<uint32_t>(order / 4),
    };
}

/// Appends the PCM Media Info Block, its channel-order string NUL padded to a word.
void append_pcm_media_info(BufferSerializerBuilder& writer, IpmxPcmMediaInfo const& pcm) noexcept
{
    constexpr std::array<uint8_t, 3> ZEROS{};
    size_t const order = padded_to_word(pcm.channel_order.size());
    writer.append(pcm_media_header(pcm, order)).append(make_const_span(pcm.channel_order));
    writer.append(std::span<uint8_t const>{ZEROS}.first(order - pcm.channel_order.size()));
}

/// The bytes a block's length field (32-bit words less one) gives it, its header included.
[[nodiscard]] auto block_length(ieee::doublet_t const length) noexcept -> size_t
{
    return (size_t{length.get()} + 1) * 4;
}

/// The Info Block's fixed part; std::nullopt when the tag is wrong or its length is out of range.
[[nodiscard]] auto parse_info_block_header(std::span<uint8_t const> const extension) noexcept -> std::optional<IpmxInfoBlockHeader>
{
    IpmxInfoBlockHeader header{};
    BufferDeserializerBuilder reader{extension};
    reader.parse(&header);
    if (!reader || header.tag.get() != IPMX_TAG || block_length(header.length) < IpmxInfoBlockHeader::LENGTH ||
        block_length(header.length) > extension.size()) {
        return std::nullopt;
    }
    return header;
}

/// A Media Info Block: its type, and its bytes, header included.
struct MediaInfoBlock
{
    uint16_t type{0};
    std::span<uint8_t const> bytes;
};

/// The Media Info Block starting `rest`, a type and a length in words less one; std::nullopt when it is
/// truncated or overruns `rest`.
[[nodiscard]] auto next_media_info_block(std::span<uint8_t const> const rest) noexcept -> std::optional<MediaInfoBlock>
{
    BufferDeserializerBuilder reader{rest};
    ieee::doublet_t type{};
    ieee::doublet_t length{};
    reader.parse(&type).parse(&length);
    if (!reader || block_length(length) > rest.size()) {
        return std::nullopt;
    }
    return MediaInfoBlock{.type = type.get(), .bytes = rest.first(block_length(length))};
}

/// A PCM Media Info Block; std::nullopt when its fixed part or channel-order string overruns it.
[[nodiscard]] auto parse_pcm_media_info(std::span<uint8_t const> const media_bytes) noexcept -> std::optional<IpmxPcmMediaInfo>
{
    IpmxPcmMediaInfoHeader media{};
    BufferDeserializerBuilder reader{media_bytes};
    reader.parse(&media);
    size_t const order = size_t{media.channel_order_words.get()} * 4;
    if (!reader || media_bytes.size() - IpmxPcmMediaInfoHeader::LENGTH < order) {
        return std::nullopt;
    }
    return IpmxPcmMediaInfo{
        .sampling_rate = media.sampling_rate.get(),
        .sample_size = media.sample_size.get(),
        .channel_count = media.channel_count.get(),
        .packet_time_us = media.packet_time.get(),
        .measured_sample_rate = media.measured_sample_rate.get(),
        .channel_order = unpad(media_bytes.subspan(IpmxPcmMediaInfoHeader::LENGTH, order)),
    };
}

/// Walks the Media Info Blocks after the fixed part, keeping the first PCM one; false when one is malformed.
[[nodiscard]] auto parse_media_info_blocks(std::span<uint8_t const> const info_block, IpmxInfoBlock& block) noexcept -> bool
{
    size_t at = IpmxInfoBlockHeader::LENGTH;
    while (at < info_block.size()) {
        auto const media = next_media_info_block(info_block.subspan(at));
        if (!media) {
            return false;
        }
        if (media->type == IPMX_MEDIA_PCM && !block.pcm) {
            block.pcm = parse_pcm_media_info(media->bytes);
            if (!block.pcm) {
                return false;
            }
        }
        at += media->bytes.size();
    }
    return true;
}

}  // namespace

auto ipmx_info_block_size(IpmxInfoBlock const& block) noexcept -> size_t
{
    size_t size = IpmxInfoBlockHeader::LENGTH;
    if (block.pcm) {
        size += IpmxPcmMediaInfoHeader::LENGTH + padded_to_word(block.pcm->channel_order.size());
    }
    return size;
}

auto serialize_ipmx_info_block(IpmxInfoBlock const& block, std::span<uint8_t> const out) noexcept -> std::optional<size_t>
{
    size_t const size = ipmx_info_block_size(block);
    if (block.ts_refclk.size() > IpmxInfoBlockHeader::TS_REFCLK_LENGTH ||
        block.mediaclk.size() > IpmxInfoBlockHeader::MEDIACLK_LENGTH || size / 4 > 0x10000 || out.size() < size) {
        return std::nullopt;
    }
    BufferSerializerBuilderWithBuffer writer{out};
    writer.append(info_block_header(block, size));
    if (block.pcm) {
        append_pcm_media_info(writer, *block.pcm);
    }
    return writer ? std::optional{writer.position()} : std::nullopt;
}

auto parse_ipmx_info_block(std::span<uint8_t const> const extension) noexcept -> std::optional<IpmxInfoBlock>
{
    auto const header = parse_info_block_header(extension);
    if (!header) {
        return std::nullopt;
    }
    IpmxInfoBlock block{
        .version = header->version.get(),
        .ts_refclk = unpad(extension.subspan(offsetof(IpmxInfoBlockHeader, ts_refclk), IpmxInfoBlockHeader::TS_REFCLK_LENGTH)),
        .mediaclk = unpad(extension.subspan(offsetof(IpmxInfoBlockHeader, mediaclk), IpmxInfoBlockHeader::MEDIACLK_LENGTH)),
        .pcm = std::nullopt,
    };
    if (!parse_media_info_blocks(extension.first(block_length(header->length)), block)) {
        return std::nullopt;
    }
    return block;
}

}  // namespace statusbar::rtp
