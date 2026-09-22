#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace XPMF {

/**
 * @brief Knows which material objects Community Shaders' True PBR has a configuration for
 *
 * True PBR gives a material object physically based parameters (base color scale, roughness,
 * specular level, glint) through a file of its own, Data\PBRMaterialObjects\<EditorID>.json, and
 * applies them to the PBR shapes of every static that carries the material - on top of the
 * material's color and the projected textures, which its shader takes the way vanilla's does.
 * Such a material is somebody else's: whoever wrote the file tuned it against the record as it is
 * - color, single pass switch and all - and against textures with nothing baked in, so this
 * plugin leaves the record alone unless a profile says it is for PBR materials ("pbr"), which
 * makes its textures PBR ones. The shapes under it are still shapes with projected snow on them
 * either way, and get their vertex colors and their roof shelter.
 *
 * What counts is decided exactly the way Community Shaders decides it (TruePBR.cpp,
 * PNState::ReadPBRRecordConfigs and TruePBR::GetPBRMaterialObjectData): the files directly inside
 * the folder, extension ".json" as written, file name without the extension compared to the
 * EditorID character for character. A file for an EditorID in another case is no configuration to
 * Community Shaders, and so none here. The one liberty taken is that a file is not parsed: one
 * Community Shaders rejects as malformed still counts, which errs on the side of leaving a
 * material alone.
 *
 * Without Community Shaders in the process the files mean nothing and are ignored.
 */
class PbrMaterialObjects {
public:
    PbrMaterialObjects() = delete;

    /**
     * @brief Looks for Community Shaders and reads the configuration folder; kDataLoaded or later,
     * when every SKSE plugin is in the process
     */
    static void load();

    /**
     * @brief Whether Community Shaders is loaded
     */
    [[nodiscard]] static auto isCommunityShadersLoaded() -> bool;

    /**
     * @brief Whether True PBR has a configuration for a material object
     *
     * @param editorId The material's EditorID, as loaded
     * @return bool Always false before load() and without Community Shaders
     */
    [[nodiscard]] static auto contains(std::string_view editorId) -> bool;

private:
    constexpr static const wchar_t* COMMUNITY_SHADERS_MODULE = L"CommunityShaders"; /**< CommunityShaders.dll */
    constexpr static const char* CONFIG_FOLDER = R"(Data\PBRMaterialObjects)"; /**< Relative to the game root,
                                                                                 the working directory */
    constexpr static std::string_view CONFIG_EXTENSION = ".json";

    /**
     * @brief Transparent hash so lookups take a string_view without building a string
     */
    struct Hash {
        using is_transparent = void;
        [[nodiscard]] auto operator()(std::string_view text) const noexcept -> std::size_t
        {
            return std::hash<std::string_view> {}(text);
        }
    };

    static inline bool s_communityShaders = false;
    static inline std::unordered_set<std::string, Hash, std::equal_to<>> s_editorIds; /**< File names without
                                                                                        their extension */
};

} // namespace XPMF
