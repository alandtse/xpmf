#pragma once

#include <string>
#include <string_view>

namespace XPMF {

/**
 * @brief The one string operation the plugin needs in more than one place
 *
 * EditorIDs, resource paths and profile file names are plain 8 bit strings the game compares
 * without regard to case, and ASCII in every case that matters, so an ASCII fold is all the
 * lower casing there is; any other byte is left as it is.
 */
class Text {
public:
    Text() = delete;

    /**
     * @brief ASCII lower case
     */
    [[nodiscard]] static auto toLower(std::string_view text) -> std::string
    {
        std::string lowered(text);
        for (char& ch : lowered) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        return lowered;
    }
};

} // namespace XPMF
