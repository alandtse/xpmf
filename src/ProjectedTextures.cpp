#include "ProjectedTextures.hpp"

#include "TextureColor.hpp"

#include "PCH.h"

#include <d3d11.h>

#include <DirectXTex.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace XPMF;

void ProjectedTextures::install()
{
    // The first of BSLightingShader's vtables is the BSShader one SetupGeometry lives in
    REL::Relocation<std::uintptr_t> vtable {RE::VTABLE_BSLightingShader[0]};
    SetupGeometryHook::s_func = vtable.write_vfunc(SetupGeometryHook::SLOT, SetupGeometryHook::thunk);
    spdlog::info("Lighting shader SetupGeometry hook installed");
}

auto ProjectedTextures::add(const Paths& paths) -> std::optional<Added>
{
    if (s_active.load(std::memory_order_acquire)) {
        spdlog::error("Projected textures were asked for after the draw hook went live; nothing was loaded");
        return std::nullopt; // the sets must not change under a running hook
    }
    if (paths == Paths {}) {
        return Added {}; // the game's textures throughout: nothing to tell draws apart for
    }
    for (std::size_t set = 0; set < s_count; ++set) {
        if (s_sets.at(set).paths == paths) {
            return Added {.set = set, .ownNoise = s_sets.at(set).noise != nullptr};
        }
    }
    if (s_count >= MAX_SETS) {
        spdlog::error("More than {} different sets of projected textures; {} gets none", MAX_SETS, paths.diffuse);
        return std::nullopt;
    }

    Set fresh {.paths = paths};
    if (!paths.diffuse.empty()) {
        fresh.diffuse = paths.pbr ? holdForPbr(paths.diffuse) : hold(paths.diffuse);
        if (fresh.diffuse == nullptr) {
            return std::nullopt;
        }
    }
    const auto optional = [](const std::string& path, const char* what) -> TexturePointer* {
        if (path.empty()) {
            return nullptr;
        }
        auto* const holder = hold(path);
        if (holder == nullptr) {
            spdlog::warn("...so the game's projected {} stays", what);
        }
        return holder;
    };
    fresh.normal = optional(paths.normal, "normal");
    fresh.noise = optional(paths.noise, "noise");
    fresh.detailNormal = optional(paths.detailNormal, "detail normal");
    if (fresh.diffuse == nullptr && fresh.normal == nullptr && fresh.noise == nullptr
        && fresh.detailNormal == nullptr) {
        return Added {}; // nothing loaded that would be swapped in
    }

    const std::size_t set = s_count;
    s_sets.at(set) = std::move(fresh);
    ++s_count;
    const auto orGame = [](const std::string& path, const TexturePointer* holder) -> const char* {
        return holder != nullptr ? path.c_str() : "(the game's)";
    };
    const Set& added = s_sets.at(set);
    spdlog::info("Projected texture set {}: diffuse {}{}, normal {}, noise {}, detail normal {}",
                 set + 1,
                 orGame(paths.diffuse, added.diffuse),
                 paths.pbr ? " (for PBR)" : "",
                 orGame(paths.normal, added.normal),
                 orGame(paths.noise, added.noise),
                 orGame(paths.detailNormal, added.detailNormal));
    return Added {.set = set, .ownNoise = added.noise != nullptr};
}

auto ProjectedTextures::tag(std::size_t set,
                            const RE::NiColor& color) -> RE::NiColor
{
    // Bit patterns in the lowest mantissa bits: the value hardly moves (see the class), the
    // pattern is exact, and both code paths that copy the color into a shape copy it bit for bit
    const auto mark = [](float channel, std::uint32_t mask, std::uint32_t bits) -> float {
        return std::bit_cast<float>((std::bit_cast<std::uint32_t>(channel) & ~mask) | bits);
    };
    return {mark(color.red, MARK_MASK, RED_MARK),
            mark(color.green, MARK_MASK, GREEN_MARK),
            mark(color.blue, SET_MASK, static_cast<std::uint32_t>(set) + 1U)};
}

void ProjectedTextures::activate()
{
    if (s_count == 0 || s_active.load(std::memory_order_acquire)) {
        return;
    }
    s_active.store(true, std::memory_order_release);
    spdlog::info("{} projected texture sets are live; every material without one keeps the game's textures", s_count);
}

auto ProjectedTextures::hold(const std::string& dataPath) -> TexturePointer*
{
    // Exactly how the engine loads its own projected textures, at startup and from its
    // ReloadProjectedUVTextures console command: a demand load
    RE::NiPointer<RE::NiTexture> texture;
    RE::BSShaderManager::GetTexture(dataPath.c_str(), true, texture, false);
    auto* const source = texture != nullptr ? netimmerse_cast<RE::NiSourceTexture*>(texture.get()) : nullptr;
    if (source == nullptr || source->rendererTexture == nullptr) {
        spdlog::warn("{} did not load as a renderer texture", dataPath);
        return nullptr;
    }
    return new TexturePointer {source}; // NOLINT(cppcoreguidelines-owning-memory): never freed on purpose
}

auto ProjectedTextures::holdForPbr(const std::string& dataPath) -> TexturePointer*
{
    // The engine's texture first: it is the one the landscape samples, so the copy below is
    // measured against it, and it lends the copy everything but its pixels
    TexturePointer* const original = hold(dataPath);
    if (original == nullptr) {
        return nullptr;
    }

    const auto bytes = TextureColor::readResource(dataPath);
    DirectX::TexMetadata metadata {};
    DirectX::ScratchImage image;
    if (bytes.empty()
        || FAILED(DirectX::LoadFromDDSMemory(bytes.data(), bytes.size(), DirectX::DDS_FLAGS_NONE, &metadata, image))) {
        spdlog::warn("{} could not be read back as a DDS; it is projected as the engine loaded it", dataPath);
        return original;
    }
    if (!DirectX::IsSRGB(metadata.format)) {
        spdlog::warn("{} is not in an sRGB format (DXGI format {}), which Community Shaders' PBR expects of a base "
                     "color; it is projected as it is, and may not match the landscape",
                     dataPath,
                     static_cast<std::uint32_t>(metadata.format));
        return original;
    }

    // The same pixels in the format's non-sRGB counterpart: sampled, they come out as stored,
    // and the shader's own decode is the only one they get
    auto* const device = reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice());
    ID3D11ShaderResourceView* view = nullptr;
    const HRESULT result = device != nullptr ? DirectX::CreateShaderResourceViewEx(device,
                                                                                   image.GetImages(),
                                                                                   image.GetImageCount(),
                                                                                   metadata,
                                                                                   D3D11_USAGE_IMMUTABLE,
                                                                                   D3D11_BIND_SHADER_RESOURCE,
                                                                                   0,
                                                                                   0,
                                                                                   DirectX::CREATETEX_IGNORE_SRGB,
                                                                                   &view)
                                             : E_FAIL;
    if (FAILED(result) || view == nullptr) {
        spdlog::warn("{} could not be created as a non-sRGB texture (HRESULT {:#010x}); it is projected as the engine "
                     "loaded it",
                     dataPath,
                     static_cast<std::uint32_t>(result));
        return original;
    }
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource); // a reference of its own, kept for good like the view's

    // The engine's side of a texture: what the render pass binds
    const DXGI_FORMAT format = DirectX::MakeLinear(metadata.format);
    auto* const rendererTexture = new RE::NiTexture::RendererData(static_cast<std::uint16_t>(metadata.width),
                                                                  static_cast<std::uint16_t>(metadata.height));
    rendererTexture->texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(resource);
    rendererTexture->resourceView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(view);
    rendererTexture->unk1C = static_cast<std::uint8_t>(metadata.mipLevels); // mips
    rendererTexture->unk1D = static_cast<std::uint8_t>(format); // format, in the engine's byte

    // ...under a source texture of our own. Nothing in the engine builds one for a plugin, so it
    // is the engine's own, copied: same class, same name, same format preferences, and pixels of
    // ours. It is on no list (a texture's neighbors are the engine's bookkeeping, and a texture
    // that is never destroyed never has to leave one), it has no stream (that is for loading),
    // and its name is a reference of its own on the string pool's entry. It is never destroyed.
    auto* const clone = static_cast<RE::NiSourceTexture*>(RE::malloc(sizeof(RE::NiSourceTexture)));
    if (clone == nullptr) {
        return original;
    }
    std::memcpy(static_cast<void*>(clone), static_cast<const void*>(original->get()), sizeof(RE::NiSourceTexture));
    std::construct_at(&clone->name, (*original)->name);
    clone->prev = nullptr;
    clone->next = nullptr;
    clone->resourceStream = nullptr;
    clone->rendererTexture = reinterpret_cast<RE::BSGraphics::Texture*>(rendererTexture);

    spdlog::info("{}: {}x{}, {} mips, sRGB DXGI format {} - projected for PBR as its non-sRGB counterpart, format {}",
                 dataPath,
                 metadata.width,
                 metadata.height,
                 metadata.mipLevels,
                 static_cast<std::uint32_t>(metadata.format),
                 static_cast<std::uint32_t>(format));
    return new TexturePointer {clone}; // NOLINT(cppcoreguidelines-owning-memory): never freed on purpose
}

auto ProjectedTextures::setOf(const RE::NiColorA& color) -> std::size_t
{
    // The signature first: two twelve bit patterns no arithmetic lands on. Then the set, whose
    // byte an untagged color that happens to carry the signature has 0 in - which wraps to a
    // number far beyond s_count
    if ((std::bit_cast<std::uint32_t>(color.red) & MARK_MASK) != RED_MARK
        || (std::bit_cast<std::uint32_t>(color.green) & MARK_MASK) != GREEN_MARK) {
        return NOT_TAGGED;
    }
    const std::uint32_t set = (std::bit_cast<std::uint32_t>(color.blue) & SET_MASK) - 1U;
    return set < s_count ? set : NOT_TAGGED;
}

auto ProjectedTextures::samplesProjectedTextures(std::uint32_t lightingType) -> bool
{
    constexpr std::uint32_t PARALLAX = 3;
    constexpr std::uint32_t FACEGEN = 4;
    constexpr std::uint32_t FACEGEN_RGB_TINT = 5;
    constexpr std::uint32_t HAIR = 6; // the engine binds nothing projected for it in the first place
    constexpr std::uint32_t MULTILAYER_PARALLAX = 11;
    constexpr std::uint32_t MULTI_INDEX_SPARKLE = 14;
    return lightingType != PARALLAX && lightingType != FACEGEN && lightingType != FACEGEN_RGB_TINT
        && lightingType != HAIR && lightingType != MULTILAYER_PARALLAX && lightingType != MULTI_INDEX_SPARKLE;
}

void ProjectedTextures::SetupGeometryHook::thunk(RE::BSLightingShader* shader,
                                                 RE::BSRenderPass* pass,
                                                 std::uint32_t renderFlags)
{
    // This runs for every lit draw of every frame; all but the last few lines are about leaving
    // as early and as cheaply as possible
    if (!s_active.load(std::memory_order_acquire) || pass == nullptr || pass->shaderProperty == nullptr
        || !pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kProjectedUV)) {
        s_func(shader, pass, renderFlags);
        return;
    }
    // Only lighting properties are drawn by this shader, and only they have a projection color
    const auto* const property = static_cast<const RE::BSLightingShaderProperty*>(pass->shaderProperty);
    auto* const state = RE::BSGraphics::State::GetSingleton();
    const std::size_t tagged = state != nullptr ? setOf(property->projectedUVColor) : NOT_TAGGED;
    if (tagged == NOT_TAGGED) {
        s_func(shader, pass, renderFlags);
        return;
    }

    // For the length of this one call the engine's projected textures are the profile's. Whether
    // to bind them at all (the setting, cube map passes), into which slots, with which sampler
    // state, and whether the state cache already holds them all stay the engine's decisions - and
    // the next projected draw finds the game's textures where they always were. The swaps move
    // pointers only; between them the holders keep the game's textures alive.
    const Set& set = s_sets.at(tagged);
    const bool surface = samplesProjectedTextures(((pass->passEnum - LIGHTING_TECHNIQUE_START) >> LIGHTING_TYPE_SHIFT)
                                                  & LIGHTING_TYPE_MASK);
    const auto swapAll = [&]() -> void {
        if (set.noise != nullptr) {
            std::swap(state->defaultTextureProjNoiseMap, *set.noise);
        }
        if (set.detailNormal != nullptr) {
            std::swap(state->defaultTextureProjNormalDetailMap, *set.detailNormal);
        }
        if (surface && set.diffuse != nullptr) {
            std::swap(state->defaultTextureProjDiffuseMap, *set.diffuse);
        }
        if (surface && set.normal != nullptr) {
            std::swap(state->defaultTextureProjNormalMap, *set.normal);
        }
    };
    swapAll();
    s_func(shader, pass, renderFlags);
    swapAll();
}
