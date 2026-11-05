module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <share.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

module ngine.core;

import :console;

namespace ngine::log::detail {

// Die Konsole von Windows nutzt sonst die OEM-Codepage (z. B. 850) und zeigt die UTF-8-Bytes
// unserer Log-Zeilen als zwei falsche Zeichen. Ohne Konsole (umgeleitete Ausgabe, CI) schlägt
// der Aufruf fehl und ändert nichts; die Bytes bleiben UTF-8.
void prepare_console() noexcept {
    SetConsoleOutputCP(CP_UTF8);
}

// fopen versteht unter Windows kein UTF-8; _wfsopen nimmt den Pfad als UTF-16 und lässt andere
// Programme mitlesen.
std::FILE* open_log_file(std::string_view utf8_path) noexcept {
    try {
        const std::filesystem::path path{std::u8string{utf8_path.begin(), utf8_path.end()}};
        return _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    } catch (...) {
        return nullptr;
    }
}

} // namespace ngine::log::detail
