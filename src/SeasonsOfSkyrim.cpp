#include "SeasonsOfSkyrim.hpp"

#include "PCH.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

using namespace XPMF;

auto SeasonsOfSkyrim::isLoaded() -> bool { return REX::W32::GetModuleHandleW(MODULE) != nullptr; }

auto SeasonsOfSkyrim::isSinglePassMaterial(std::string_view editorId) -> bool
{
    // EditorIDs are not case sensitive to the game, and ASCII
    return std::ranges::equal(editorId, SINGLE_PASS_MATERIAL, [](char lhs, char rhs) -> bool {
        const auto lower
            = [](char ch) -> char { return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch; };
        return lower(lhs) == lower(rhs);
    });
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
