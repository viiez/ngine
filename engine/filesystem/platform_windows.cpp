module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

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

// Über GetModuleFileNameW; der Puffer wächst, bis der Pfad hineinpasst.
std::expected<std::string, std::string> platform_executable_path() {
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::unexpected(std::string{"Pfad der Anwendung unbekannt"});
        }
        if (length < buffer.size()) {
            const std::u8string utf8 =
                std::filesystem::path{std::wstring{buffer.data(), length}}.u8string();
            return std::string{utf8.begin(), utf8.end()};
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace ngine::filesystem
