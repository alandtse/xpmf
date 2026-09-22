#include "SeasonsOfSkyrim.hpp"

#include "Text.hpp"

#include "PCH.h"

#include <cstdint>
#include <string_view>

using namespace XPMF;

auto SeasonsOfSkyrim::isLoaded() -> bool { return REX::W32::GetModuleHandleW(MODULE) != nullptr; }

auto SeasonsOfSkyrim::isSinglePassMaterial(std::string_view editorId) -> bool
{
    // EditorIDs are not case sensitive to the game
    return Text::toLower(editorId) == Text::toLower(SINGLE_PASS_MATERIAL);
}

auto SeasonsOfSkyrim::hasWinterSnow(const RE::NiAVObject& root) -> bool
{
    // By name rather than through NiObjectNET::HasExtraData: that takes a BSFixedString, and this
    // runs for every clone and every gathered reference - no reason to go through the string pool
    // for a root that has two or three extra data at most
    if (root.extra == nullptr) {
        return false;
    }
    for (std::uint16_t index = 0; index < root.extraDataSize; ++index) {
        const auto* const extra = root.extra[index]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        if (extra != nullptr && extra->GetName() == TAG) {
            return true;
        }
    }
    return false;
}
