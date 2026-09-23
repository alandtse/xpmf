#pragma once

#include "PCH.h"

#include <string_view>

namespace XPMF {

/**
 * @brief What this plugin has to know about powerofthree's Seasons of Skyrim to work with it
 *
 * In winter Seasons of Skyrim snows over statics, movable statics and containers that have no
 * snow of their own, from its TESBoundObject::Clone3D hooks (vtable slot 0x40 of TESObjectSTAT,
 * BGSMovableStatic and TESObjectCONT, written at kPostLoad). Read off its source (SnowSwap.h /
 * .cpp, Cache.cpp), there are two ways it does that:
 *
 *  - Multipass, for whitelisted models and with its "prefer multipass" setting: it points the
 *    static's materialObj at SOS_WIN_SnowMaterialObjectMP for the length of the engine's Clone3D.
 *    A multipass material is nothing this plugin touches, the engine's own or that one.
 *
 *  - Single pass, for everything else: it lets the engine clone the 3D and then calls
 *    NiAVObject::SetProjectedUVData on the root itself - the function the engine applies a single
 *    pass material object with - and tags the root with a NiBooleanExtraData named
 *    SOS_SNOW_SHADER. The static's materialObj is not involved, so nothing about the base form
 *    says that its clone carries snow; the tag on the 3D does.
 *
 * The values it projects are SOS_WIN_SnowMaterialObjectSP's (SnowOverSkyrim.esp), but not read
 * when they are needed: at kDataLoaded it first copies the color of the vanilla
 * SnowMaterialObject1P into that record, then keeps a copy of the record's color and falloff
 * values of its own, and every clone gets that copy. Its kDataLoaded handler runs before this
 * plugin's (SKSE loads po3_SeasonsOfSkyrim.dll before XPMF.dll, and messages go out
 * in load order), so the copy is of the color the record had before MaterialMatcher matched it:
 * the record ends up with the profile's tagged white like every other snow material, and the
 * winter snow goes on being projected in the old color - untagged, so without the profile's
 * textures, next to vanilla snow that has them.
 *
 * Hence the two things done about it, both in ProjectedGeometry. A clone carrying the tag is
 * treated as standing under SOS_WIN_SnowMaterialObjectSP, whatever its base form says: it gets
 * that material's profile's vertex colors and roof shelter. And its projection color - and its
 * falloff values, where the profile overrides them - are brought in line with what the record
 * says now (unless the record was left untouched - a True PBR configuration, patchMaterial off),
 * which is what makes its draws the profile's.
 * ProjectedGeometry puts its Clone3D hooks in at kPostPostLoad for this, one message after
 * Seasons of Skyrim's, so that they wrap its hooks and see the snow it just put on; the cell pass
 * does the same for a reference it finds tagged, should the order ever be the other way around.
 */
class SeasonsOfSkyrim {
public:
    SeasonsOfSkyrim() = delete;

    /**
     * @brief Whether Seasons of Skyrim is in the process (po3_SeasonsOfSkyrim.dll)
     *
     * Decides whether the Clone3D hooks for movable statics and containers are worth having:
     * nothing but its winter snow projects anything onto those.
     */
    [[nodiscard]] static auto isLoaded() -> bool;

    /**
     * @brief Whether an EditorID is that of the record its single pass winter snow takes its values from
     */
    [[nodiscard]] static auto isSinglePassMaterial(std::string_view editorId) -> bool;

    /**
     * @brief Whether Seasons of Skyrim put its single pass snow on a reference's 3D
     *
     * @param root The 3D's root, which is where it leaves its tag
     */
    [[nodiscard]] static auto hasWinterSnow(const RE::NiAVObject& root) -> bool;

private:
    constexpr static const wchar_t* MODULE = L"po3_SeasonsOfSkyrim";
    constexpr static std::string_view SINGLE_PASS_MATERIAL = "SOS_WIN_SnowMaterialObjectSP";
    constexpr static std::string_view TAG = "SOS_SNOW_SHADER"; /**< Name of the NiBooleanExtraData on a snowed root */
};

} // namespace XPMF
