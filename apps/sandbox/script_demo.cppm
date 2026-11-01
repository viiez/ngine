module;

#include <ngine/profile.hpp>

#include <chrono>
#include <string>
#include <vector>

// Durchstich D2: .NET starten, Skripte aus Sandbox.Scripts.dll laden, einige Frames lang Daten
// der Engine aus C# ändern, Aufrufkosten messen und die Skripte wie beim Hot Reload neu laden.
export module sandbox.script_demo;

import ngine.core;
import ngine.profile;
import ngine.script;

namespace sandbox {

using namespace ngine;

export int run_script_demo(u64 frames, const std::string& dotnet_dir) {
    if (dotnet_dir.empty()) {
        log::error("Die Sandbox wurde ohne .NET-SDK gebaut (NGINE_WITH_DOTNET=OFF)");
        return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    auto runtime = script::ScriptRuntime::create({.assembly_dir = dotnet_dir});
    if (!runtime) {
        log::error("{}", runtime.error());
        return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    log::info(".NET gestartet in {:.1f} ms",
              std::chrono::duration<f64, std::milli>(started - start).count());

    const std::string assembly = dotnet_dir + "/Sandbox.Scripts.dll";
    std::vector<script::Position> positions(1000);
    for (int round = 0; round < 2; ++round) {
        const auto load_start = std::chrono::steady_clock::now();
        const auto count = (*runtime)->load_scripts(assembly);
        if (!count) {
            log::error("{}", count.error());
            return 1;
        }
        log::info(
            "{} Skript(e) geladen in {:.1f} ms", *count,
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - load_start)
                .count());
        if (auto result = (*runtime)->start(); !result) {
            log::error("{}", result.error());
            return 1;
        }

        f64 total_ms = 0.0;
        for (u64 frame = 0; frame < frames; ++frame) {
            const auto frame_start = std::chrono::steady_clock::now();
            if (auto result = (*runtime)->update(1.0f / 60.0f, positions); !result) {
                log::error("{}", result.error());
                return 1;
            }
            total_ms += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() -
                                                               frame_start)
                            .count();
            profile::frame_mark();
        }
        log::info("Update der Skripte für {} Positionen: im Mittel {:.3f} ms pro Frame",
                  positions.size(), total_ms / static_cast<f64>(frames == 0 ? 1 : frames));
        log::info("Position 0 nach {} Frames: ({:.3f}, {:.3f}, {:.3f})", frames, positions[0].x,
                  positions[0].y, positions[0].z);

        // Entladen wie vor einem Hot Reload; die zweite Runde lädt dieselbe Assembly neu.
        const auto unloaded = (*runtime)->unload_scripts();
        if (!unloaded || !*unloaded) {
            log::error("Die Skripte wurden nicht freigegeben");
            return 1;
        }
    }

    constexpr u32 calls = 1'000'000;
    log::info("Ein Aufruf C++ -> C# kostet {:.1f} ns, C# -> C++ {:.1f} ns",
              (*runtime)->measure_calls_to_managed(calls),
              (*runtime)->measure_calls_to_native(calls));
    return 0;
}

} // namespace sandbox
