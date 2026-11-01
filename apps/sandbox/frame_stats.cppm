module;

#include <array>

// Werte, die die Sandbox in jedem Frame an den Profiler meldet.
export module sandbox.frame_stats;

import ngine.core;
import ngine.memory;
import ngine.profile;

namespace sandbox {

namespace {

using ngine::memory::Category;

// Tracy verlangt Plot-Namen, die statisch leben. Reihenfolge wie in memory::Category.
constexpr std::array<const char*, ngine::memory::category_count> plot_names{
    "Speicher Allgemein (Bytes)", "Speicher Rendering (Bytes)", "Speicher Welt (Bytes)",
    "Speicher Jobs (Bytes)",      "Speicher Scripting (Bytes)", "Speicher Netzwerk (Bytes)",
    "Speicher Audio (Bytes)",     "Speicher Werkzeuge (Bytes)",
};
static_assert(static_cast<ngine::usize>(Category::Tools) + 1 == plot_names.size());

} // namespace

// Meldet den aktuellen Verbrauch jeder Speicherkategorie als Tracy-Plot. Einmal pro Frame aufrufen.
export void report_memory() noexcept {
    if constexpr (ngine::profile::enabled) {
        for (ngine::usize i = 0; i < plot_names.size(); ++i) {
            const auto stats = ngine::memory::stats(static_cast<Category>(i));
            ngine::profile::plot(plot_names[i], static_cast<ngine::f64>(stats.current));
        }
    }
}

} // namespace sandbox
