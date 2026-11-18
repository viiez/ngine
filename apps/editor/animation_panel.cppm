module;

#include <format>
#include <string>
#include <vector>

// Fenster „Animation“ des Editors (R4.6): Automat des Animators an der gewählten Entity oder ihrem
// ersten Nachkommen mit Animator, aktiver Zustand, Überblendung und Parameter mit Schiebern zum
// Ausprobieren, auch außerhalb des Spiels. Dazu das Skelett aller Figuren mit Pose im Viewport.
// Den Automaten bearbeitet man als Datei (.animgraph) mit Hot Reload; einen grafischen Editor gibt
// es nicht (Abstimmung zu R4.6).
export module editor.animation;

import ngine.core;
import ngine.animation;
import ngine.editor_ui;
import ngine.math;
import ngine.render;
import ngine.world;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

export constexpr const char* animation_window = "Animation";

// Entity mit Animator: entity selbst oder der erste Nachkomme (Breitensuche), 0 ohne.
export u64 animator_entity(world::World& world, u64 entity) {
    if (entity == 0 || !world.alive(entity)) {
        return 0;
    }
    const world::ComponentId animator = world.component<animation::Animator>();
    std::vector<u64> level{entity};
    for (u32 depth = 0; depth < 4 && !level.empty(); ++depth) {
        std::vector<u64> next;
        for (const u64 id : level) {
            if (world.has(id, animator)) {
                return id;
            }
            const std::vector<u64> children = world.children(id);
            next.insert(next.end(), children.begin(), children.end());
        }
        level = std::move(next);
    }
    return 0;
}

export class AnimationPanel {
public:
    // selected ist die Kennung der gewählten Entity in der Welt (0: keine). Beim Wechsel zwischen
    // Spielen und Bearbeiten vergisst das System seine Automaten, damit Werte aus dem Spiel nicht
    // in der Bearbeitung bleiben.
    void draw(animation::AnimationSystem& system, world::World& world, u64 selected, bool playing,
              bool& open) {
        if (playing != was_playing_) {
            system.clear();
            was_playing_ = playing;
        }
        if (!open) {
            return;
        }
        if (ui::begin_window(animation_window, &open)) {
            draw_contents(system, world, selected);
        }
        ui::end_window();
    }

    // Skelette aller Figuren mit Pose: Linien von jedem Knoten zu seinem Elternknoten.
    void draw_skeletons(render::DebugDraw& debug, world::World& world,
                        const animation::AnimationSystem& system) const {
        if (!show_skeletons_) {
            return;
        }
        world.each<const world::WorldTransform, const animation::Animator>(
            [&](world::Entity entity, const world::WorldTransform& transform,
                const animation::Animator&) {
                const auto view = system.view(entity.id());
                const auto nodes = system.model_space(entity.id());
                if (!view || view->rig == nullptr || nodes.empty()) {
                    return;
                }
                const auto position = [&](usize node) {
                    const math::Mat4& m = nodes[node];
                    return transform.value.transform_point(
                        math::to_f64(math::Vec3{m[3].x, m[3].y, m[3].z}));
                };
                for (usize node = 0; node < nodes.size() && node < view->rig->parents.size();
                     ++node) {
                    const i32 parent = view->rig->parents[node];
                    if (parent < 0) {
                        continue;
                    }
                    debug.line(position(static_cast<usize>(parent)), position(node),
                               {1.0f, 0.55f, 0.1f});
                }
            });
    }

private:
    void draw_contents(animation::AnimationSystem& system, world::World& world, u64 selected) {
        ui::checkbox("Skelette zeigen", show_skeletons_);
        const animation::AnimationStats& stats = system.stats();
        ui::text(std::format("{} Animatoren, {} laufen, {} mit Pose, {} warten ({:.3f} ms)",
                             stats.animators, stats.running, stats.posed, stats.waiting,
                             stats.update_ms));
        ui::separator();
        const u64 entity = animator_entity(world, selected);
        if (entity == 0) {
            ui::text_wrapped("Keine Entity mit Animator gewählt (auch nicht darunter).");
            return;
        }
        const auto view = system.view(entity);
        if (!view) {
            ui::text_wrapped("Der Animator läuft noch nicht (erstes Bild nach dem Anlegen).");
            return;
        }
        if (!view->error.empty()) {
            ui::text_colored({1.0f, 0.4f, 0.3f, 1.0f}, view->error);
        }
        if (view->machine == nullptr || !view->machine->valid()) {
            ui::text_wrapped("Automat oder Modell laden noch.");
            return;
        }
        const animation::StateMachine& machine = *view->machine;
        ui::text(std::format("Zustand: {}", machine.state()));
        if (!machine.previous_state().empty() || machine.fade() < 1.0f) {
            ui::progress_bar(
                machine.fade(),
                std::format("Überblendung von {}", machine.previous_state().empty()
                                                       ? std::string{"(eingefroren)"}
                                                       : std::string{machine.previous_state()}));
        }
        ui::progress_bar(machine.phase(), "Zeit im Zustand");
        ui::text(std::format("Pose in diesem Bild: {}", view->posed ? "ja" : "nein"));
        ui::separator();
        ui::text("Parameter");
        for (usize i = 0; i < machine.parameter_count(); ++i) {
            const std::string name{machine.parameter_name(i)};
            ui::push_id(name);
            switch (machine.parameter_type(i)) {
            case animation::ParameterType::Float: {
                f32 value = machine.parameter_value(i);
                ui::set_next_item_width(160.0f);
                if (ui::drag_scalars(name, ui::DataType::F32, &value, 1, 0.05f, nullptr, nullptr,
                                     "%.2f")) {
                    system.set_float(entity, name, value);
                }
                break;
            }
            case animation::ParameterType::Bool: {
                bool value = machine.parameter_value(i) != 0.0f;
                if (ui::checkbox(name, value)) {
                    system.set_bool(entity, name, value);
                }
                break;
            }
            case animation::ParameterType::Trigger:
                if (ui::button(name)) {
                    system.set_trigger(entity, name);
                }
                ui::same_line();
                ui::text_disabled(machine.parameter_value(i) != 0.0f ? "(wartet)" : "(Auslöser)");
                break;
            }
            ui::pop_id();
        }
    }

    bool show_skeletons_ = false;
    bool was_playing_ = false;
};

} // namespace editor
