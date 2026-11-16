module;

#include <array>
#include <format>
#include <initializer_list>
#include <string>

// Fenster „Audio“ des Editors (R4.5): Gerät, Lautstärke und Stummschaltung der Busse, Stimmen und
// Last des Mixers mit Leerläufen und zu späten Blöcken (Knacken), dazu die Reichweiten der
// Klangquellen im Viewport (innen min_distance, außen max_distance).
export module editor.audio;

import ngine.core;
import ngine.audio;
import ngine.editor_ui;
import ngine.math;
import ngine.render;
import ngine.world;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

export constexpr const char* audio_window = "Audio";

export class AudioPanel {
public:
    void draw(audio::AudioEngine* engine, audio::AudioSystem* system, bool& open) {
        if (!open) {
            return;
        }
        if (ui::begin_window(audio_window, &open)) {
            if (engine == nullptr) {
                ui::text_wrapped("Kein Audio.");
            } else {
                draw_engine(*engine, system);
            }
        }
        ui::end_window();
    }

    // Reichweiten aller Quellen mit Ortung, wenn im Fenster eingeschaltet.
    void draw_ranges(render::DebugDraw& debug, world::World& world) const {
        if (!show_ranges_) {
            return;
        }
        world.each<const world::WorldTransform, const audio::AudioSource>(
            [&](const world::WorldTransform& transform, const audio::AudioSource& source) {
                if (!source.spatial) {
                    return;
                }
                debug.sphere(transform.position(), source.min_distance, {1.0f, 0.75f, 0.2f}, 24);
                debug.sphere(transform.position(), source.max_distance, {0.35f, 0.6f, 1.0f}, 32);
            });
    }

private:
    void draw_engine(audio::AudioEngine& engine, audio::AudioSystem* system) {
        ui::text(std::format("Gerät: {} ({} Hz)", engine.device().name(), engine.sample_rate()));
        ui::separator();
        constexpr std::array buses{audio::AudioBus::Master, audio::AudioBus::Music,
                                   audio::AudioBus::Effects, audio::AudioBus::Ambient,
                                   audio::AudioBus::Interface};
        constexpr f64 min_volume = 0.0;
        constexpr f64 max_volume = 1.0;
        for (const audio::AudioBus bus : buses) {
            ui::push_id(std::string{audio::to_string(bus)});
            f32 volume = engine.bus_volume(bus);
            ui::set_next_item_width(160.0f);
            if (ui::drag_scalars(audio::to_string(bus), ui::DataType::F32, &volume, 1, 0.005f,
                                 &min_volume, &max_volume, "%.2f")) {
                engine.set_bus_volume(bus, volume);
            }
            ui::same_line();
            bool muted = engine.bus_muted(bus);
            if (ui::checkbox("stumm", muted)) {
                engine.set_bus_muted(bus, muted);
            }
            ui::pop_id();
        }

        ui::separator();
        const audio::AudioStats stats = engine.stats();
        ui::text(std::format("Stimmen: {} (hörbar {}, virtuell {}), Ströme {}", stats.mixer.active,
                             stats.mixer.audible, stats.mixer.virtual_voices,
                             stats.streams.streams));
        ui::progress_bar(static_cast<f32>(stats.mixer.load),
                         std::format("Last {:.0f} %", stats.mixer.load * 100.0));
        ui::text(std::format("Block: {:.3f} ms, höchstens {:.3f} ms", stats.mixer.last_block_ms,
                             stats.mixer.max_block_ms));
        const UiProblem underruns{stats.mixer.underruns, "Leerläufe der Ströme"};
        const UiProblem dropped{stats.mixer.dropped_commands, "verworfene Befehle"};
        const UiProblem failures{stats.streams.failures, "gescheiterte Ströme"};
        for (const UiProblem& problem : {underruns, dropped, failures}) {
            const std::string line = std::format("{}: {}", problem.label, problem.count);
            if (problem.count == 0) {
                ui::text(line);
            } else {
                ui::text_colored({.r = 1.0f, .g = 0.4f, .b = 0.4f}, line);
            }
        }
        if (system != nullptr) {
            const audio::AudioSystemStats world_stats = system->stats();
            ui::text(std::format("Quellen in der Welt: {}, spielen {}, Einmal-Klänge {}, laden {}",
                                 world_stats.sources, world_stats.playing, world_stats.one_shots,
                                 world_stats.pending));
        }
        ui::separator();
        ui::checkbox("Reichweiten im Viewport", show_ranges_);
    }

    struct UiProblem {
        u64 count;
        const char* label;
    };

    bool show_ranges_ = false;
};

} // namespace editor
