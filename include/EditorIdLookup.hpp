#pragma once

#include "PCH.h"

#include <cstdint>
#include <string>

namespace XPMF {

/**
 * @brief Reads form EditorIDs out of po3's Tweaks, a hard requirement of this plugin
 *
 * The EditorID is what identifies a snow material (see MaterialClassifier), and the game does not
 * keep it: BGSMaterialObject::Load hands the EDID string to the form's SetFormEditorID virtual,
 * which BGSMaterialObject leaves as the TESForm stub ("mov al, 1; ret"), and the class has no
 * member that could hold it - so GetFormEditorID returns "" for a material with or without
 * Tweaks installed.
 *
 * Tweaks ("Load EditorIDs", on by default) hooks that SetFormEditorID slot for the form types
 * that drop their name, BGSMaterialObject among them, and files the strings in a map of its
 * own keyed by FormID. It shares that map through a single exported function,
 *
 *     extern "C" const char* GetFormEditorID(std::uint32_t formID);   // po3_Tweaks.dll
 *
 * which is what this class resolves and calls - the same route CLibUtil's
 * editorID::get_editorID takes for SPID, KID and friends.
 */
class EditorIdLookup {
public:
    EditorIdLookup() = delete;

    /**
     * @brief Whether po3's Tweaks is loaded and exposes its EditorID cache
     *
     * Resolved on first use and remembered, so the first call has to come after every SKSE
     * plugin has been loaded (kDataLoaded is fine, the SKSE load callback is not: whether
     * po3_Tweaks.dll is in the process by then depends on plugin load order).
     *
     * @return bool False when the DLL or its export is missing
     */
    [[nodiscard]] static auto isAvailable() -> bool;

    /**
     * @brief Looks up the EditorID a form was loaded with
     *
     * @param form The form to look up
     * @return std::string The EditorID, or empty when the form has none on record - a record
     *         without an EDID, a form created at runtime (Tweaks skips dynamic forms), Tweaks
     *         missing, or its "Load EditorIDs" setting turned off
     */
    [[nodiscard]] static auto find(const RE::TESForm* form) -> std::string;

private:
    using GetFormEditorId_t = const char* (*)(std::uint32_t formId); /**< Signature of the Tweaks export */
    constexpr static const wchar_t* TWEAKS_MODULE = L"po3_Tweaks"; /**< po3_Tweaks.dll */
    constexpr static const char* TWEAKS_EXPORT = "GetFormEditorID"; /**< The one export of its EditorID cache */

    /**
     * @brief Resolves po3_Tweaks.dll!GetFormEditorID once
     *
     * @return GetFormEditorId_t The export, or nullptr when Tweaks is not loaded
     */
    [[nodiscard]] static auto resolve() -> GetFormEditorId_t;
};

} // namespace XPMF
