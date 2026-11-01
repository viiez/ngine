module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

module ngine.script;

import :native;

namespace ngine::script {

namespace {

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}

} // namespace

void* load_library(const std::string& path, std::string& error) {
    const std::filesystem::path native{std::u8string{path.begin(), path.end()}};
    HMODULE library = LoadLibraryW(native.c_str());
    if (library == nullptr) {
        error = std::format("LoadLibraryW fehlgeschlagen (Fehler {})", GetLastError());
    }
    return static_cast<void*>(library);
}

void* find_symbol(void* library, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
}

const char* hostfxr_file_name() {
    return "hostfxr.dll";
}

std::vector<std::string> dotnet_roots() {
    std::vector<std::string> roots;
    wchar_t buffer[MAX_PATH];
    if (const DWORD length = GetEnvironmentVariableW(L"DOTNET_ROOT", buffer, MAX_PATH);
        length > 0 && length < MAX_PATH) {
        roots.push_back(to_utf8(std::filesystem::path{buffer}));
    }
    if (const DWORD length = GetEnvironmentVariableW(L"ProgramFiles", buffer, MAX_PATH);
        length > 0 && length < MAX_PATH) {
        roots.push_back(to_utf8(std::filesystem::path{buffer} / "dotnet"));
    }
    return roots;
}

} // namespace ngine::script
