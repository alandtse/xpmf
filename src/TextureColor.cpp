#include "TextureColor.hpp"

#include "PCH.h"

#include <DirectXMath.h>
#include <DirectXTex.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace XPMF;

auto TextureColor::meanColor(const std::string& dataPath) -> std::optional<RE::NiColor>
{
    const auto bytes = readResource(dataPath);
    if (bytes.empty()) {
        spdlog::error("Texture {} was not found (loose or in any archive)", dataPath);
        return std::nullopt;
    }

    DirectX::TexMetadata metadata {};
    DirectX::ScratchImage texture;
    HRESULT result
        = DirectX::LoadFromDDSMemory(bytes.data(), bytes.size(), DirectX::DDS_FLAGS_NONE, &metadata, texture);
    if (FAILED(result)) {
        spdlog::error("Texture {} is not a DDS this plugin can read (HRESULT {:#010x})",
                      dataPath,
                      static_cast<std::uint32_t>(result));
        return std::nullopt;
    }

    // Largest mip within the texel budget; a texture without mips is measured at full size
    std::size_t mip = 0;
    const DirectX::Image* image = texture.GetImage(mip, 0, 0);
    while (image != nullptr && image->width * image->height > MAX_TEXELS && mip + 1 < metadata.mipLevels) {
        image = texture.GetImage(++mip, 0, 0);
    }
    if (image == nullptr) {
        spdlog::error("Texture {} holds no image data", dataPath);
        return std::nullopt;
    }

    // Block compressed formats (nearly every game texture) have to be unpacked first
    DirectX::ScratchImage unpacked;
    if (DirectX::IsCompressed(image->format)) {
        result = DirectX::Decompress(*image, DXGI_FORMAT_UNKNOWN, unpacked);
        if (FAILED(result)) {
            spdlog::error("Texture {} could not be decompressed (format {}, HRESULT {:#010x})",
                          dataPath,
                          static_cast<std::uint32_t>(image->format),
                          static_cast<std::uint32_t>(result));
            return std::nullopt;
        }
        image = unpacked.GetImage(0, 0, 0);
    }

    // Decompress keeps the _SRGB-ness of the source format, so this covers both paths
    const bool linearize = DirectX::IsSRGB(image->format);

    std::array<double, 3> sum {};
    std::size_t texels = 0;
    result = DirectX::EvaluateImage(
        *image, [&](const DirectX::XMVECTOR* pixels, std::size_t width, std::size_t /*row*/) -> void {
            for (const DirectX::XMVECTOR& stored : std::span {pixels, width}) {
                const DirectX::XMVECTOR pixel = linearize ? DirectX::XMColorSRGBToRGB(stored) : stored;
                sum[0] += static_cast<double>(DirectX::XMVectorGetX(pixel));
                sum[1] += static_cast<double>(DirectX::XMVectorGetY(pixel));
                sum[2] += static_cast<double>(DirectX::XMVectorGetZ(pixel));
            }
            texels += width;
        });
    if (FAILED(result) || texels == 0) {
        spdlog::error("Texture {} could not be evaluated (format {}, HRESULT {:#010x})",
                      dataPath,
                      static_cast<std::uint32_t>(image->format),
                      static_cast<std::uint32_t>(result));
        return std::nullopt;
    }

    const auto count = static_cast<double>(texels);
    const RE::NiColor mean {
        static_cast<float>(sum[0] / count), static_cast<float>(sum[1] / count), static_cast<float>(sum[2] / count)};
    spdlog::info("Texture {}: {}x{} format {}{}, mean of mip {} = ({:.4f}, {:.4f}, {:.4f})",
                 dataPath,
                 metadata.width,
                 metadata.height,
                 static_cast<std::uint32_t>(metadata.format),
                 linearize ? " (sRGB, linearized)" : "",
                 mip,
                 mean.red,
                 mean.green,
                 mean.blue);
    return mean;
}

auto TextureColor::readResource(const std::string& dataPath) -> std::vector<std::uint8_t>
{
    RE::BSResourceNiBinaryStream stream {dataPath};
    if (!stream.good()) {
        return {};
    }

    const std::uint32_t size = stream.stream->totalSize;
    std::vector<std::uint8_t> bytes(size);
    if (size == 0 || !stream.read(bytes.data(), size)) {
        return {};
    }
    return bytes;
}
