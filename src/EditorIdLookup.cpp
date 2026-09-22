#include "EditorIdLookup.hpp"

#include "PCH.h"

#include <string>

using namespace XPMF;

auto EditorIdLookup::isAvailable() -> bool { return resolve() != nullptr; }

auto EditorIdLookup::find(const RE::TESForm* form) -> std::string
{
    const auto getFormEditorId = resolve();
    if (getFormEditorId == nullptr || form == nullptr) {
        return {};
    }

    // Tweaks answers an unknown FormID with an empty string rather than a null pointer; the
    // check only guards against that ever changing
    const char* const editorId = getFormEditorId(form->GetFormID());
    return editorId != nullptr ? std::string {editorId} : std::string {};
}

auto EditorIdLookup::resolve() -> GetFormEditorId_t
{
    static const GetFormEditorId_t s_getFormEditorId = []() -> GetFormEditorId_t {
        const auto tweaks = REX::W32::GetModuleHandleW(L"po3_Tweaks");
        if (tweaks == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<GetFormEditorId_t>(REX::W32::GetProcAddress(tweaks, "GetFormEditorID"));
    }();
    return s_getFormEditorId;
}
