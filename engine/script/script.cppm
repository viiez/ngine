module;

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// C#-Scripting über eingebettetes .NET (ADR 0006, ADR 0023). Durchstich D2: Laufzeit über hostfxr
// starten, Skripte aus einer Assembly in einen entladbaren Kontext laden, Aufrufe in beide
// Richtungen. Läuft ohne Fenster und GPU.
export module ngine.script;

import ngine.core;

export namespace ngine::script {

template <typename T>
using Result = std::expected<T, std::string>;

// Gleiches Layout wie NGine.Position in C#. Daten, die beide Seiten teilen, sind blittable.
struct Position {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
};

struct RuntimeDesc {
    // Ordner mit NGine.Scripting.dll und NGine.Scripting.runtimeconfig.json.
    std::string assembly_dir;
    // Installationsordner von .NET. Leer: DOTNET_ROOT, dann die üblichen Orte des Systems.
    std::string dotnet_root;
};

// Die .NET-Laufzeit im Prozess. Es gibt höchstens eine; .NET lässt sich nicht entladen und im
// selben Prozess nicht mit anderen Einstellungen neu starten. Gehört einem Thread.
class ScriptRuntime {
public:
    [[nodiscard]] static Result<std::unique_ptr<ScriptRuntime>> create(const RuntimeDesc& desc);

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;
    ~ScriptRuntime();

    // Lädt alle Skripte (Klassen mit NGine.IScript) aus einer Assembly. Liefert ihre Anzahl.
    [[nodiscard]] Result<u32> load_scripts(std::string_view assembly_path);
    // Stoppt die Skripte und entlädt ihre Assembly. true, wenn .NET den Kontext freigegeben hat.
    [[nodiscard]] Result<bool> unload_scripts();

    [[nodiscard]] Result<void> start();
    // Skripte sehen und ändern positions direkt, ohne Kopie.
    [[nodiscard]] Result<void> update(f32 delta_time, std::span<Position> positions);

    // Prüft den Weg C++ → C# → C++.
    [[nodiscard]] i32 add(i32 a, i32 b);
    // Mittlere Kosten eines Aufrufs in Nanosekunden.
    [[nodiscard]] f64 measure_calls_to_managed(u32 count);
    [[nodiscard]] f64 measure_calls_to_native(u32 count);

    // Nur für create(); Impl ist außerhalb des Moduls nicht bekannt.
    struct Impl;
    explicit ScriptRuntime(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::script
