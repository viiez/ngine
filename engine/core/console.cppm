module;

#include <cstdio>
#include <string_view>

// Interne Partition: Vorbereitung der Konsole für die Log-Ausgabe und Öffnen der Log-Datei.
// Plattformcode steht in console_windows.cpp bzw. console_posix.cpp.
module ngine.core:console;

namespace ngine::log::detail {

// Stellt die Konsole auf UTF-8 um, damit Umlaute richtig erscheinen. Unter Linux ist sie es schon.
void prepare_console() noexcept;

// Legt eine Datei zum Schreiben neu an (Pfad in UTF-8); nullptr bei Fehler. Andere Programme
// dürfen sie lesen, während die Engine schreibt.
[[nodiscard]] std::FILE* open_log_file(std::string_view utf8_path) noexcept;

} // namespace ngine::log::detail
