#pragma once

#include "PCH.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace XPMF {

/**
 * @brief Computes the average color of a texture as a shader would sample it
 *
 * The file is read through the game's own resource system, so it is found wherever the game
 * would find it - a loose file, a BSA, or a mod manager's virtual file system - and the
 * texture that wins in the user's load order is the one that gets measured.
 *
 * "As a shader would sample it" means the stored values, not a color managed reading of them:
 * Skyrim samples its UNORM textures without any gamma decode and lights in that space, which is
 * the same space a material's single pass color is used in. The one exception is a texture
 * saved in an _SRGB format, which the GPU linearizes on read; those are linearized here too so
 * the average still equals what the landscape shader receives.
 */
class TextureColor {
public:
    TextureColor() = delete;

    /**
     * @brief Averages the RGB channels of a DDS texture
     *
     * @param dataPath Path relative to Data, e.g. textures\landscape\snow01landscape.dds
     * @return std::optional<RE::NiColor> The mean color, or std::nullopt (after logging why)
     *         when the file is missing or cannot be decoded
     */
    [[nodiscard]] static auto meanColor(const std::string& dataPath) -> std::optional<RE::NiColor>;

    /**
     * @brief Reads a whole file through BSResource
     *
     * @param dataPath Path relative to Data
     * @return std::vector<std::uint8_t> The file contents, or empty when it could not be opened
     */
    [[nodiscard]] static auto readResource(const std::string& dataPath) -> std::vector<std::uint8_t>;

private:
    constexpr static std::size_t MAX_TEXELS = std::size_t {2048} * 2048; /**< Largest mip worth decoding. A mip is a
                                                                            box filter of the one above, which
                                                                            keeps the mean to within the block
                                                                            compression's rounding (0.4% on the
                                                                            vanilla snow), so 2K textures are
                                                                            measured whole and only 4K / 8K ones
                                                                            pay that for a decode four to sixteen
                                                                            times shorter */
};

} // namespace XPMF
