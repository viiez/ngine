module;

#include <memory>
#include <string>

// .NET-Laufzeit der Anwendungen (R4.4): eine pro Prozess, Assemblies im Ordner „dotnet“ neben der
// Anwendung (TD-12).
export module app.scripting;

import ngine.core;
import ngine.filesystem;
import ngine.script;

namespace app {

using namespace ngine;

// Startet .NET; ohne Laufzeit oder Assemblies nullptr mit Warnung, die Anwendung läuft dann ohne
// Skripte. Muss die Szenen überleben, die Skripte nutzen.
export std::unique_ptr<script::ScriptRuntime> start_script_runtime() {
    const auto exe = filesystem::executable_dir();
    if (!exe) {
        log::warn("Skripte: {}", exe.error());
        return nullptr;
    }
    auto created =
        script::ScriptRuntime::create({.assembly_dir = filesystem::join(*exe, "dotnet")});
    if (!created) {
        log::warn("Skripte: .NET startet nicht ({}); es laufen keine Skripte", created.error());
        return nullptr;
    }
    return *std::move(created);
}

} // namespace app
