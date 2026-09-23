#include "ProjectedVertexData.hpp"

#include "Offsets.hpp"
#include "VertexLayout.hpp"

#include "PCH.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

using namespace XPMF;

namespace {

/**
 * @brief FNV-1a over a byte range, continuing from a previous hash
 */
auto hashBytes(std::span<const std::uint8_t> bytes,
               std::uint64_t hash) -> std::uint64_t
{
    constexpr std::uint64_t PRIME = 0x100000001B3ULL;
    for (const auto byte : bytes) {
        hash = (hash ^ byte) * PRIME;
    }
    return hash;
}

} // namespace

auto ProjectedVertexData::shared(const Shape& shape) -> Data*
{
    if (shape.source == nullptr) {
        return nullptr;
    }
    // Colors the shader ignores tint nothing and mask nothing - to it the alpha is 1 already - so
    // such a shape needs no variant. One that shows them gets its alpha reset where the profile
    // neutralizes the alpha (unless it is transparency), and its colors whitened where it
    // neutralizes the colors
    if (!shape.colorsEnabled) {
        addRef(shape.source);
        return shape.source;
    }
    const bool resetAlpha = shape.neutralizeAlpha && !shape.keepAlpha;
    if (!resetAlpha && !shape.neutralize) {
        addRef(shape.source); // the mesh's colors and alpha stay what they are
        return shape.source;
    }

    const SharedKey key {.source = shape.source, .whiten = shape.neutralize, .resetAlpha = resetAlpha};
    const std::scoped_lock lock(s_lock);
    if (const auto found = s_shared.find(key); found != s_shared.end()) {
        addRef(found->second);
        return found->second;
    }

    // Built with the lock held: two loader threads cloning the same model at once would
    // otherwise both build it, and a buffer creation is quick
    Recipe recipe;
    recipe.whiten = shape.neutralize;
    recipe.resetAlpha = resetAlpha;

    Data* const variant = build(shape, recipe);
    if (variant == nullptr) {
        return nullptr;
    }
    if (variant != shape.source) {
        // Fresh from the engine with a count of 1: that one is the registry's. The registry also
        // pins the source, whose address is the key the variant is found by.
        addRef(shape.source);
        s_variants.emplace(variant, Entry {.source = shape.source, .isShared = true});
    } else {
        addRef(shape.source); // "nothing to change" is remembered too; this pin is the shared map's
    }
    s_shared.emplace(key, variant);
    addRef(variant); // the caller's
    return variant;
}

auto ProjectedVertexData::custom(const Shape& shape,
                                 std::span<const std::uint8_t> values) -> Data*
{
    if (shape.source == nullptr || values.size() != shape.vertexCount) {
        return nullptr;
    }
    {
        const std::scoped_lock lock(s_lock);
        if (s_privateBytes >= K_BUDGET) {
            return nullptr;
        }
    }

    // Only a shape whose alpha is free to be written gets this far (see ShelterMap::judge), so
    // it starts from the mesh's own unless the profile neutralizes it or the shader never showed it
    Recipe recipe;
    recipe.whiten = !shape.colorsEnabled || shape.neutralize;
    recipe.resetAlpha = !shape.colorsEnabled || shape.neutralizeAlpha;
    recipe.values = values;
    Data* const variant = build(shape, recipe);
    if (variant == nullptr) {
        return nullptr;
    }
    if (variant == shape.source) {
        addRef(variant);
        return variant;
    }

    const auto layout = VertexLayout::from(variant->vertexDesc);
    const std::size_t bytes = layout.has_value() ? static_cast<std::size_t>(layout->stride) * shape.vertexCount : 0;
    addRef(shape.source);
    addRef(variant); // the caller's, next to the registry's initial one
    const std::scoped_lock lock(s_lock);
    s_variants.emplace(variant,
                       Entry {.source = shape.source,
                              .bytes = bytes,
                              .fingerprint = fingerprint(values),
                              .colorless = !shape.colorsEnabled});
    s_privateBytes += bytes;
    return variant;
}

auto ProjectedVertexData::sourceOf(Data* data) -> Data*
{
    const std::scoped_lock lock(s_lock);
    const auto found = s_variants.find(data);
    return found != s_variants.end() ? found->second.source : data;
}

auto ProjectedVertexData::isForColorlessShape(const Data* data) -> bool
{
    const std::scoped_lock lock(s_lock);
    const auto found = s_variants.find(data);
    return found != s_variants.end() && found->second.colorless;
}

auto ProjectedVertexData::fingerprintOf(const Data* data) -> std::uint64_t
{
    const std::scoped_lock lock(s_lock);
    const auto found = s_variants.find(data);
    return found != s_variants.end() ? found->second.fingerprint : 0;
}

auto ProjectedVertexData::fingerprint(std::span<const std::uint8_t> values) -> std::uint64_t
{
    constexpr std::uint64_t OFFSET_BASIS = 0xCBF29CE484222325ULL;
    const std::uint64_t hash = hashBytes(values, OFFSET_BASIS);
    return hash != 0 ? hash : 1; // 0 means "not a private variant"
}

void ProjectedVertexData::addRef(Data* data)
{
    if (data != nullptr) {
        // The engine's own release is a lock xadd on this field
        _InterlockedIncrement(reinterpret_cast<volatile long*>(&data->refCount)); // NOLINT
    }
}

void ProjectedVertexData::release(Data* data)
{
    if (data == nullptr) {
        return;
    }
    // BSTriShape::~BSTriShape releases renderer data through vfunc 5 of the geometry buffer
    // manager; using the same path keeps this symmetric with the engine's allocator
    static const REL::Relocation<void**> managerAddress {Offsets::K_GEOMETRY_BUFFER_MANAGER};
    void* const manager = *managerAddress; // NOLINT
    if (manager == nullptr) {
        return; // leaking one buffer set beats calling into a dead manager
    }
    const auto* const vtbl = *reinterpret_cast<Offsets::ReleaseRendererData_t* const*>(manager);
    constexpr std::size_t RELEASE_VFUNC = 5;
    vtbl[RELEASE_VFUNC](manager, data); // NOLINT
}

void ProjectedVertexData::install(RE::BSTriShape& shape,
                                  Data* data)
{
    auto& geometry = shape.GetGeometryRuntimeData();
    Data* const previous = geometry.rendererData;
    if (data == nullptr || data == previous) {
        release(data);
        return;
    }
    geometry.rendererData = data;
    geometry.vertexDesc = data->vertexDesc; // differs only where a color was appended
    release(previous);
}

void ProjectedVertexData::collectGarbage()
{
    std::vector<Data*> dead;
    std::vector<const Data*> deadVariants;
    {
        // A count of 1 means only this registry is left holding on. Nothing can raise it in the
        // meantime: shared() needs the lock, and the engine only copies renderer data from a
        // shape that holds it, of which there are none.
        const std::scoped_lock lock(s_lock);
        std::erase_if(s_variants, [&](const auto& item) -> bool {
            auto* const variant = const_cast<Data*>(item.first); // NOLINT: the registry owns it
            if (variant->refCount > 1) {
                return false;
            }
            if (!item.second.isShared) {
                s_privateBytes -= std::min(s_privateBytes, item.second.bytes);
            }
            deadVariants.push_back(variant);
            dead.push_back(variant);
            dead.push_back(item.second.source);
            return true;
        });
        std::erase_if(s_shared, [&](const auto& item) -> bool {
            Data* const data = item.second;
            if (std::ranges::find(deadVariants, data) != deadVariants.end()) {
                return true; // a variant that just went; the registry's reference was its only one
            }
            if (data != item.first.source || data->refCount > 1) {
                return false;
            }
            dead.push_back(data); // an identical-to-source entry whose model has been unloaded
            return true;
        });
    }
    // Outside the lock: a release may free D3D buffers
    for (auto* const data : dead) {
        release(data);
    }
}

auto ProjectedVertexData::SharedKeyHash::operator()(const SharedKey& key) const noexcept -> std::size_t
{
    constexpr std::size_t WHITEN_SALT = 0x9E3779B97F4A7C15ULL; /**< So the keys of one source never collide */
    constexpr std::size_t RESET_SALT = 0x517CC1B727220A95ULL;
    return std::hash<const Data*> {}(key.source) ^ (key.whiten ? WHITEN_SALT : 0) ^ (key.resetAlpha ? RESET_SALT : 0);
}

auto ProjectedVertexData::build(const Shape& shape,
                                const Recipe& recipe) -> Data*
{
    const Data& source = *shape.source;
    const auto sourceLayout = VertexLayout::from(source.vertexDesc);
    if (!sourceLayout.has_value() || source.rawVertexData == nullptr || shape.vertexCount == 0) {
        return nullptr;
    }

    // A mesh without a color attribute gets one appended (white, until the recipe says otherwise)
    RE::BSGraphics::VertexDesc desc = source.vertexDesc;
    const auto layout = sourceLayout->hasColors ? sourceLayout : sourceLayout->withColors(desc);
    if (!layout.has_value()) {
        return nullptr;
    }

    const std::span<const std::uint8_t> input {source.rawVertexData,
                                               static_cast<std::size_t>(sourceLayout->stride) * shape.vertexCount};
    std::vector<std::uint8_t> output(static_cast<std::size_t>(layout->stride) * shape.vertexCount);
    bool changed = !sourceLayout->hasColors;
    for (std::uint32_t index = 0; index < shape.vertexCount; ++index) {
        const auto from = input.subspan(static_cast<std::size_t>(index) * sourceLayout->stride, sourceLayout->stride);
        const auto to = std::span {output}.subspan(static_cast<std::size_t>(index) * layout->stride, layout->stride);
        std::memcpy(to.data(), from.data(), from.size());

        const auto color = to.subspan(layout->colorOffset, VertexLayout::COLOR_SIZE);
        if (!sourceLayout->hasColors) {
            std::ranges::fill(color, VertexLayout::COLOR_MAX);
        }
        const std::array<std::uint8_t, VertexLayout::COLOR_SIZE> before {color[0], color[1], color[2], color[3]};

        constexpr std::size_t RGB = 3; /**< The color's channels before the alpha */
        if (recipe.whiten) {
            std::ranges::fill(color.first(RGB), VertexLayout::COLOR_MAX);
        }
        if (recipe.resetAlpha) {
            color[RGB] = VertexLayout::COLOR_MAX;
        }
        if (!recipe.values.empty()) {
            // The alpha scaled, rounded to nearest: a mask the mesh's author painted is (where it
            // was kept) only ever lowered further, and a reset one becomes the shelter's alone
            const std::uint32_t scaled = static_cast<std::uint32_t>(color[RGB]) * recipe.values[index];
            color[RGB] = static_cast<std::uint8_t>((scaled + (VertexLayout::COLOR_MAX / 2)) / VertexLayout::COLOR_MAX);
        }
        changed = changed || !std::ranges::equal(before, color);
    }
    if (!changed) {
        return shape.source;
    }

    auto* const renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (renderer == nullptr) {
        return nullptr;
    }
    static const REL::Relocation<Offsets::CreateTriShapeData_t> createTriShapeData {Offsets::K_CREATE_TRISHAPE_DATA};
    Data* const variant = createTriShapeData(renderer,
                                             output.data(),
                                             static_cast<std::uint32_t>(output.size()),
                                             std::bit_cast<std::uint64_t>(desc),
                                             &shape.source->indexBuffer);
    if (variant == nullptr) {
        return nullptr;
    }

    // The engine leaves the CPU index list empty; the decal builder walks it, and so does this
    // plugin's own occluder gather. From RE::malloc because the engine's release frees it.
    if (source.rawIndexData != nullptr && shape.triangleCount > 0) {
        const std::size_t indexBytes = static_cast<std::size_t>(shape.triangleCount) * 3 * sizeof(std::uint16_t);
        if (auto* const indices = static_cast<std::uint16_t*>(RE::malloc(indexBytes)); indices != nullptr) {
            std::memcpy(indices, source.rawIndexData, indexBytes);
            variant->rawIndexData = indices;
        }
    }
    return variant;
}
