module;

#include <cstdlib>
#include <expected>
#include <string>

module ngine.filesystem;

import :platform;

namespace ngine::filesystem {

// Nach der XDG Base Directory Specification: $XDG_DATA_HOME, wenn es ein absoluter Pfad ist,
// sonst ~/.local/share. Pfade unter Linux sind Bytes; wir nehmen sie als UTF-8.
std::expected<std::string, std::string> platform_user_data_dir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] == '/') {
        return std::string{xdg};
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::string{home} + "/.local/share";
    }
    return std::unexpected(std::string{"Weder XDG_DATA_HOME noch HOME ist gesetzt"});
}

} // namespace ngine::filesystem
