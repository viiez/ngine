module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <expected>
#include <filesystem>
#include <string>

module ngine.filesystem;

import :platform;

namespace ngine::filesystem {

// Roaming-AppData, z. B. C:\Users\<Name>\AppData\Roaming.
std::expected<std::string, std::string> platform_user_data_dir() {
    PWSTR folder = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &folder);
    if (FAILED(result)) {
        CoTaskMemFree(folder);
        return std::unexpected(std::string{"AppData-Ordner nicht gefunden"});
    }
    const std::u8string utf8 = std::filesystem::path{folder}.u8string();
    CoTaskMemFree(folder);
    return std::string{utf8.begin(), utf8.end()};
}

} // namespace ngine::filesystem
