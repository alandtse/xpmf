#pragma once

#include "PCH.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace XPMF {

/**
 * @brief Where the attributes this plugin reads and writes sit inside one packed BSTriShape vertex
 *
 * A BSGraphics vertex descriptor is a 64 bit value: the low nibble is the vertex size in dwords,
 * the nibbles after it are the attribute offsets in dwords (position, uv, uv2, normal, tangent,
 * color, skinning, land data, eye data, in that order), and bits 44 and up say which attributes
 * exist. A static's vertex is position (three floats plus a fourth slot holding a bitangent
 * component), uv, normal (three unsigned bytes mapping 0..255 to -1..1, plus a bitangent byte),
 * tangent, and an RGBA color.
 *
 * The position is always float in Skyrim. VF_FULLPREC and half precision positions are
 * Fallout 4's: every vanilla SSE mesh has the flag clear (0x03B on a typical static) with the 32
 * byte stride only float positions add up to, and CommonLib's VertexDesc::GetSize counts
 * VF_VERTEX as four floats unconditionally. Reading the flag as "half" - which this struct once
 * did - turns every position into noise around the model's origin.
 */
struct VertexLayout {
    std::uint32_t stride {}; /**< Bytes per vertex */
    std::uint32_t normalOffset {}; /**< Byte offset of the normal; meaningless without hasNormals */
    std::uint32_t colorOffset {}; /**< Byte offset of the RGBA color; meaningless without hasColors */
    bool hasNormals {}; /**< VF_NORMAL */
    bool hasColors {}; /**< VF_COLORS */

    constexpr static std::uint64_t SIZE_MASK = 0xF; /**< The descriptor's low nibble: vertex size in dwords */
    constexpr static std::uint32_t POSITION_SIZE = 4 * sizeof(float); /**< xyz plus the bitangent slot */
    constexpr static std::uint32_t COLOR_SIZE = 4; /**< One RGBA color */
    constexpr static std::uint8_t COLOR_MAX = 255; /**< A channel's full value: white, or an alpha of 1 */
    constexpr static std::uint32_t NORMAL_COMPONENTS = 3; /**< x, y and z, a byte each */
    constexpr static std::uint32_t NORMAL_SIZE = NORMAL_COMPONENTS + 1; /**< ...followed by a bitangent byte */
    constexpr static std::uint32_t MAX_STRIDE = 15 * sizeof(std::uint32_t); /**< What the size nibble can say */

    /**
     * @brief Reads the layout of a plain static vertex
     *
     * @param desc The shape's vertex descriptor
     * @return std::optional<VertexLayout> std::nullopt for anything that is not a rigid static
     *         vertex with a position (skinned, landscape, eye and instanced layouts keep data
     *         behind the color, and none of them carries projected snow)
     */
    [[nodiscard]] static auto from(RE::BSGraphics::VertexDesc desc) -> std::optional<VertexLayout>
    {
        using RE::BSGraphics::Vertex;
        if (!desc.HasFlag(Vertex::VF_VERTEX)
            || desc.HasFlag(static_cast<Vertex::Flags>(Vertex::VF_SKINNED | Vertex::VF_LANDDATA | Vertex::VF_EYEDATA
                                                       | Vertex::VF_INSTANCEDATA))) {
            return std::nullopt;
        }

        VertexLayout layout;
        layout.stride
            = static_cast<std::uint32_t>((std::bit_cast<std::uint64_t>(desc) & SIZE_MASK) * sizeof(std::uint32_t));
        layout.hasNormals = desc.HasFlag(Vertex::VF_NORMAL);
        layout.hasColors = desc.HasFlag(Vertex::VF_COLORS);
        layout.normalOffset = desc.GetAttributeOffset(Vertex::VA_NORMAL);
        layout.colorOffset = desc.GetAttributeOffset(Vertex::VA_COLOR);

        if (layout.stride < POSITION_SIZE || (layout.hasNormals && layout.normalOffset + NORMAL_SIZE > layout.stride)
            || (layout.hasColors && layout.colorOffset + COLOR_SIZE > layout.stride)) {
            return std::nullopt;
        }
        return layout;
    }

    /**
     * @brief The layout (and descriptor) this one becomes when a color is appended to every vertex
     *
     * The color goes behind everything else: for the rigid layouts from() accepts nothing
     * follows it in the engine's own attribute order either, and the engine builds its input
     * layouts from the offset nibbles rather than from an assumed order.
     *
     * @param desc The descriptor to extend; rewritten in place
     * @return std::optional<VertexLayout> std::nullopt when the vertex cannot grow any further
     */
    [[nodiscard]] auto withColors(RE::BSGraphics::VertexDesc& desc) const -> std::optional<VertexLayout>
    {
        if (hasColors || stride + COLOR_SIZE > MAX_STRIDE) {
            return std::nullopt;
        }
        VertexLayout grown = *this;
        grown.hasColors = true;
        grown.colorOffset = stride;
        grown.stride = stride + COLOR_SIZE;

        desc.SetFlag(RE::BSGraphics::Vertex::VF_COLORS);
        desc.SetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR, grown.colorOffset);
        desc = std::bit_cast<RE::BSGraphics::VertexDesc>((std::bit_cast<std::uint64_t>(desc) & ~SIZE_MASK)
                                                         | (grown.stride / sizeof(std::uint32_t)));
        return grown;
    }

    /**
     * @brief Model space position of a vertex
     */
    [[nodiscard]] static auto position(std::span<const std::uint8_t> vertex) -> RE::NiPoint3
    {
        RE::NiPoint3 point;
        std::memcpy(&point, vertex.data(), sizeof(point));
        return point;
    }

    /**
     * @brief Model space normal of a vertex (not renormalized; the byte encoding is close enough)
     */
    [[nodiscard]] auto normal(std::span<const std::uint8_t> vertex) const -> RE::NiPoint3
    {
        constexpr float BYTE_TO_UNIT = 2.0F / 255.0F;
        const auto bytes = vertex.subspan(normalOffset, NORMAL_COMPONENTS);
        return {(static_cast<float>(bytes[0]) * BYTE_TO_UNIT) - 1.0F,
                (static_cast<float>(bytes[1]) * BYTE_TO_UNIT) - 1.0F,
                (static_cast<float>(bytes[2]) * BYTE_TO_UNIT) - 1.0F};
    }
};

} // namespace XPMF
