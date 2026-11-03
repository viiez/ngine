module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

module ngine.core;

import :console;

namespace ngine::log::detail {

// Die Konsole von Windows nutzt sonst die OEM-Codepage (z. B. 850) und zeigt die UTF-8-Bytes
// unserer Log-Zeilen als zwei falsche Zeichen. Ohne Konsole (umgeleitete Ausgabe, CI) schlägt
// der Aufruf fehl und ändert nichts; die Bytes bleiben UTF-8.
void prepare_console() noexcept {
    SetConsoleOutputCP(CP_UTF8);
}

} // namespace ngine::log::detail
