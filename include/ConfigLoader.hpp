#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace XPMF {

/**
 * @brief Loads and serves the plugin configuration from Data/SKSE/Plugins/XPMF
 *
 * The configuration is a set of profiles. A profile names the material objects it is about -
 * wildcard patterns over their EditorIDs - and carries every setting there is for them: the
 * textures their projection shows, whether vertex colors may tint it, whether roofs keep it
 * out. Snow and ash are the two the plugin ships with; they fall from the sky alike and look
 * nothing alike, which is the whole reason a profile is the unit of configuration.
 *
 * One profile per file, every *.json in the folder XPMF next to the DLL
 * (snow.json, ash.json, and whatever anyone adds). A folder of files rather than one file with
 * a list in it so that a mod can bring a profile for its own materials without overwriting
 * anyone else's - in a mod manager the files of different mods simply end up side by side. A
 * material object that several profiles match belongs to the one that matches it most
 * specifically (MaterialClassifier: the pattern with the most literal characters, a pbr
 * profile over a general one), and among equals to the first file name in alphabetical order -
 * which is how an added profile takes a few materials out of a shipped one's hands: name them.
 *
 * JSON rather than the INI this started as: a profile holds lists, which [General] key=value
 * lines do not express. What JSON cannot hold is an explanation, so the settings are documented
 * in the README rather than in the files.
 *
 * Everything is read once at plugin load (loadConfig) into statics; the getters are plain
 * accessors and never touch the disk. Files are validated strictly: every field but the four
 * textures has to be there and has to have its type (and its range), and a file that fails is
 * rejected as a whole, with every reason in one error in the log - half a profile is not
 * something anyone asked for. A texture that is not named is not replaced. Keys that are not
 * settings only earn a warning, a "comment" key being the one way to leave a note in JSON.
 * Without the folder the built-in profiles (ash, snow) apply; with it, exactly the valid files
 * in it do - deleting ash.json is how ash is left alone.
 */
class ConfigLoader {
public:
    ConfigLoader() = delete;

    /**
     * @brief One profile: which material objects, and what is done for them
     *
     * The settings fall into the plugin's three independent parts - patching the material
     * (patchMaterial, the four textures, isSnow), neutralizeVertexColors, and roofShelter with
     * shelterFade - and any combination of them works.
     */
    struct Profile {
        std::string name; /**< For the log */
        std::string file; /**< The file it came from, lower case, for the log; empty for a built-in profile */
        std::vector<std::string> editorIds; /**< Lower case wildcard patterns (* and ?); a material object whose
                                               EditorID matches one belongs to the profile... */
        std::vector<std::string> excludeEditorIds; /**< ...unless it matches one of these as well */
        bool pbr {}; /**< ...and, when set, only if Community Shaders' True PBR has a configuration for it. The
                        one kind of profile that patches such a material object's record: its textures are
                        taken to be made for PBR */

        bool patchMaterial {}; /**< Whether the profile's material objects are changed at all */
        std::string diffuseTexture; /**< Data relative, lower case, backslashed path of the texture the projection
                                       shows; empty = not replaced, the game's ProjectedDiffuse stays */
        std::string normalTexture; /**< Same for its normal map; empty = the game's ProjectedNormal stays */
        std::string noiseTexture; /**< Same for the coverage noise; empty = the game's ProjectedNoise stays */
        std::string detailNormalTexture; /**< Same for the detail normal; empty = the game's ProjectedNormalDetail
                                            stays */
        std::optional<bool> isSnow; /**< The material objects' Snow flag; std::nullopt = as the record has it */

        bool neutralizeVertexColors {}; /**< Whether shapes that carry the projection get white vertex colors */

        bool roofShelter {}; /**< Whether vertex alpha is rewritten to keep the projection out from under cover */
        float shelterFade {}; /**< World units over which it fades out under cover */

        /**
         * @brief The name with the file it came from: two files may well share a name
         */
        [[nodiscard]] auto label() const -> std::string { return file.empty() ? name : name + " (" + file + ")"; }
    };

    /**
     * @brief The engine's own projected textures: a profile that names one of them asks for no
     * substitution, the same as not naming a texture, and they are what every draw without a
     * profile's set samples
     */
    constexpr static const char* GAME_DIFFUSE = R"(textures\effects\projecteddiffuse.dds)";
    constexpr static const char* GAME_NORMAL = R"(textures\effects\projectednormal.dds)";
    constexpr static const char* GAME_NOISE = R"(textures\effects\projectednoise.dds)";
    constexpr static const char* GAME_DETAIL_NORMAL = R"(textures\effects\projectednormaldetail.dds)";

    /**
     * @brief Loads the profiles in the XPMF folder
     */
    static void loadConfig();

    /**
     * @brief Get the profiles, in file name order - the order that breaks ties between them
     *
     * @return const std::vector<Profile>& The folder's profiles, or the built-in ones (ash, snow)
     *         when there is no folder. Stable for the life of the process: other classes keep
     *         pointers into it
     */
    [[nodiscard]] static auto getProfiles() -> const std::vector<Profile>&;

    /**
     * @brief Whether any profile patches materials, i.e. whether the draw hook is needed
     */
    [[nodiscard]] static auto isAnyMaterialPatched() -> bool;

    /**
     * @brief Whether any profile changes vertex colors or alpha, i.e. whether the Clone3D hook is needed
     */
    [[nodiscard]] static auto isAnyGeometryChanged() -> bool;

    /**
     * @brief Whether any profile keeps its projection out from under roofs, i.e. whether height maps are needed
     */
    [[nodiscard]] static auto isAnyRoofSheltered() -> bool;

    /**
     * @brief Turns whatever the user wrote for a texture into a resource system path
     *
     * @param raw UTF-8; any slashes, any case, with or without Data\ / textures\ / .dds
     * @return std::string Relative to Data, lower case, backslashes, under textures\, ending in
     *         .dds; empty for a blank value
     */
    [[nodiscard]] static auto normalizeTexturePath(std::string_view raw) -> std::string;

private:
    //
    // DEFAULT CFG VALUES
    //
    constexpr static const char* DEFAULT_SNOW_DIFFUSE = R"(textures\landscape\snow01.dds)"; /**< LSnow01, the vanilla
                                                                                            snow ground */
    constexpr static const char* DEFAULT_SNOW_NORMAL = R"(textures\landscape\snow01_n.dds)";
    constexpr static const char* DEFAULT_SNOW_PATTERN = "*snow*"; /**< Every vanilla and DLC snow material has it in
                                                                     its EditorID, and no other material does */
    constexpr static const char* DEFAULT_ASH_DIFFUSE
        = R"(textures\dlc02\landscape\volcanic_ash_01.dds)"; /**< LVolcanicAsh01, the ground of southern
                                                                Solstheim */
    constexpr static const char* DEFAULT_ASH_NORMAL = R"(textures\dlc02\landscape\volcanic_ash_01_n.dds)";
    constexpr static const char* DEFAULT_ASH_PATTERN_MATERIAL
        = "ashmaterial*"; /**< AshMaterialSolstheim1P, ...Light1P, ...Mtns1P. Anchored at the start: "*ash*" is
                             also in splash, trash and wash, and "*ashmaterial*" still in SplashMaterial */
    constexpr static const char* DEFAULT_ASH_PATTERN_DLC = "dlc2ashmaterial*"; /**< DLC2AshMaterialDusting1P */
    constexpr static const char* DEFAULT_ASH_PATTERN_LOD = "ashlodmaterial*"; /**< AshLODMaterialMtns1P */

    constexpr static float DEFAULT_SHELTER_FADE = 96.0F; /**< About how far wind carries snow in under an eave */
    constexpr static float MAX_SHELTER_FADE = 128.0F; /**< The shelter mask has been checked over 0 to 128 (0 a hard
                                                        edge, 32 and 96 in game); every covered vertex searches
                                                        this far for open sky, and past it the open vertices the
                                                        mask may thin lie farther from an eave than looks right */

    static inline std::vector<Profile> s_profiles; /**< In file name order */

    /**
     * @brief The profiles the plugin ships with, snow and ash, for an installation without the folder
     */
    [[nodiscard]] static auto builtInProfiles() -> std::vector<Profile>;
};

} // namespace XPMF
