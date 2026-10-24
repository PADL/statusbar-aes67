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

template <size_t N>
void pad_into(std::array<uint8_t, N>& field, std::string_view const text) noexcept
{
    field.fill(0);
    std::ranges::copy(text, field.begin());
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
    IpmxInfoBlockHeader header{};
    header.tag = IPMX_TAG;
    header.length = static_cast<uint16_t>((size / 4) - 1);
    header.version = block.version;
    pad_into(header.ts_refclk, block.ts_refclk);
    pad_into(header.mediaclk, block.mediaclk);

    BufferSerializerBuilderWithBuffer writer{out};
    writer.append(header);
    if (block.pcm) {
        auto const& pcm = *block.pcm;
        size_t const order = padded_to_word(pcm.channel_order.size());
        IpmxPcmMediaInfoHeader media{};
        media.type = IPMX_MEDIA_PCM;
        media.length = static_cast<uint16_t>(((IpmxPcmMediaInfoHeader::LENGTH + order) / 4) - 1);
        media.sampling_rate = pcm.sampling_rate;
        media.sample_size = pcm.sample_size;
        media.channel_count = pcm.channel_count;
        media.packet_time = pcm.packet_time_us;
        media.measured_sample_rate = pcm.measured_sample_rate;
        media.channel_order_words = static_cast<uint32_t>(order / 4);
        constexpr std::array<uint8_t, 3> ZEROS{};
        writer.append(media).append(make_const_span(pcm.channel_order));
        writer.append(std::span<uint8_t const>{ZEROS}.first(order - pcm.channel_order.size()));
    }
    if (!writer) {
        return std::nullopt;
    }
    return writer.position();
}

auto parse_ipmx_info_block(std::span<uint8_t const> const extension) noexcept -> std::optional<IpmxInfoBlock>
{
    IpmxInfoBlockHeader header{};
    BufferDeserializerBuilder reader{extension};
    reader.parse(&header);
    if (!reader || header.tag.get() != IPMX_TAG) {
        return std::nullopt;
    }
    size_t const size = (size_t{header.length.get()} + 1) * 4;
    if (size < IpmxInfoBlockHeader::LENGTH || size > extension.size()) {
        return std::nullopt;
    }
    IpmxInfoBlock block{
        .version = header.version.get(),
        .ts_refclk = unpad(extension.subspan(offsetof(IpmxInfoBlockHeader, ts_refclk), IpmxInfoBlockHeader::TS_REFCLK_LENGTH)),
        .mediaclk = unpad(extension.subspan(offsetof(IpmxInfoBlockHeader, mediaclk), IpmxInfoBlockHeader::MEDIACLK_LENGTH)),
        .pcm = std::nullopt,
    };

    // The Media Info Blocks, each a type and a length in words less one.
    size_t at = IpmxInfoBlockHeader::LENGTH;
    while (at < size) {
        if (size - at < 4) {
            return std::nullopt;
        }
        auto const rest = extension.subspan(at, size - at);
        BufferDeserializerBuilder media_reader{rest};
        ieee::doublet_t type{};
        ieee::doublet_t length{};
        media_reader.parse(&type).parse(&length);
        size_t const media_size = (size_t{length.get()} + 1) * 4;
        if (!media_reader || media_size > rest.size()) {
            return std::nullopt;
        }
        if (type.get() == IPMX_MEDIA_PCM && !block.pcm) {
            auto const media_bytes = rest.first(media_size);
            IpmxPcmMediaInfoHeader media{};
            BufferDeserializerBuilder pcm_reader{media_bytes};
            pcm_reader.parse(&media);
            size_t const order = size_t{media.channel_order_words.get()} * 4;
            if (!pcm_reader || media_bytes.size() - IpmxPcmMediaInfoHeader::LENGTH < order) {
                return std::nullopt;
            }
            block.pcm = IpmxPcmMediaInfo{
                .sampling_rate = media.sampling_rate.get(),
                .sample_size = media.sample_size.get(),
                .channel_count = media.channel_count.get(),
                .packet_time_us = media.packet_time.get(),
                .measured_sample_rate = media.measured_sample_rate.get(),
                .channel_order = unpad(media_bytes.subspan(IpmxPcmMediaInfoHeader::LENGTH, order)),
            };
        }
        at += media_size;
    }
    return block;
}

}  // namespace statusbar::rtp
