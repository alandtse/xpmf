#include "PbrMaterialObjects.hpp"

#include "PCH.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

using namespace XPMF;

void PbrMaterialObjects::load()
{
    s_editorIds.clear();
    if (REX::W32::GetModuleHandleW(COMMUNITY_SHADERS_MODULE) == nullptr) {
        spdlog::info("Community Shaders is not loaded; True PBR material object configurations play no part");
        return;
    }

    // Non-throwing throughout: a folder that cannot be listed is a folder without configurations
    std::error_code error;
    const std::filesystem::path folder {CONFIG_FOLDER};
    if (!std::filesystem::is_directory(folder, error)) {
        spdlog::info("Community Shaders is loaded; there is no {} folder, so no material object is a True PBR one",
                     CONFIG_FOLDER);
        return;
    }

    for (std::filesystem::directory_iterator entry {folder, error}, end; !error && entry != end;
         entry.increment(error)) {
        const auto& path = entry->path();
        std::error_code entryError; // one unreadable entry must not end the listing
        if (!entry->is_regular_file(entryError) || path.extension() != CONFIG_EXTENSION) {
            continue;
        }
        try {
            s_editorIds.insert(path.stem().string());
        } catch (const std::exception&) {
            // A name the ANSI code page cannot hold is a name no EditorID has
        }
    }
    if (error) {
        spdlog::warn("Listing {} stopped early: {}", CONFIG_FOLDER, error.message());
    }
    spdlog::info(
        "Community Shaders is loaded; {} has {} material object configuration(s)", CONFIG_FOLDER, s_editorIds.size());
}

auto PbrMaterialObjects::contains(std::string_view editorId) -> bool
{
    return !editorId.empty() && s_editorIds.contains(editorId);
}
