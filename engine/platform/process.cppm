module;

#include <expected>
#include <span>
#include <string>
#include <string_view>

// Externe Programme starten (R4.4: dotnet build für die Skripte). Ohne Shell; die Ausgabe von
// stdout und stderr kommt zusammen zurück. Plattformcode steht in process_posix.cpp bzw.
// process_windows.cpp.
export module ngine.platform:process;

import ngine.core;

export namespace ngine::platform {

struct ProcessResult {
    i32 exit_code = -1;
    std::string output;
};

// Startet program mit den Argumenten, wartet auf das Ende und liefert Exit-Code und Ausgabe.
// program ohne Pfad wird im PATH gesucht. Fehler nur, wenn sich das Programm nicht starten lässt;
// ein Exit-Code ungleich 0 ist kein Fehler. Blockiert: für lange Läufe in einem eigenen Thread.
[[nodiscard]] std::expected<ProcessResult, std::string>
run_process(std::string_view program, std::span<const std::string> arguments,
            std::string_view working_directory = {});

} // namespace ngine::platform
