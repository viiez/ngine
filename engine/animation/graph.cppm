module;

#include <string>
#include <string_view>
#include <vector>

export module ngine.animation:graph;

import ngine.core;
import ngine.asset;

// Zustandsautomat als Asset (R4.6, ANIM-02): eine JSON-Datei mit .meta wie die Materialtabelle,
// mit Hot Reload. Clips heißen wie die Animationen im Modell.
//
//   { "format": "ngine.animgraph", "version": 1, "model": <Asset-Kennung>, "initial": "Boden",
//     "parameters": [ { "name": "Tempo", "type": "float", "value": 0 },
//                     { "name": "Springen", "type": "trigger" } ],
//     "states": [ { "name": "Boden", "parameter": "Tempo",
//                   "blend": [ { "clip": "Stehen", "value": 0 }, { "clip": "Gehen", "value": 4 } ],
//                   "events": [ { "time": 0.0, "name": "Fuss_links" } ] },
//                 { "name": "Springen", "clip": "Springen", "loop": false, "speed": 1 } ],
//     "transitions": [ { "from": "Boden", "to": "Springen", "duration": 0.1,
//                        "conditions": [ { "parameter": "Springen", "op": "trigger" } ] },
//                      { "from": "Springen", "to": "Boden", "after_end": true } ] }
//
// "from": "*" gilt für jeden Zustand außer dem Ziel. Bedingungen: "gt" und "lt" vergleichen mit
// "value", "true" und "false" prüfen einen Schalter, "trigger" verbraucht einen Auslöser.
// Ereignisse stehen in der normierten Zeit des Zustands (0 bis 1), damit sie in einem 1D-Blend zu
// jedem Tempo passen.
export namespace ngine::animation {

inline constexpr std::string_view anim_graph_format = "ngine.animgraph";
inline constexpr i64 anim_graph_version = 1;

enum class ParameterType : u8 { Float, Bool, Trigger };

struct GraphParameter {
    std::string name;
    ParameterType type = ParameterType::Float;
    // Anfangswert (Schalter und Auslöser: 0 oder 1).
    f32 value = 0.0f;

    friend bool operator==(const GraphParameter&, const GraphParameter&) = default;
};

struct BlendPoint {
    std::string clip;
    f32 value = 0.0f;

    friend bool operator==(const BlendPoint&, const BlendPoint&) = default;
};

struct GraphEvent {
    f32 time = 0.0f;
    std::string name;

    friend bool operator==(const GraphEvent&, const GraphEvent&) = default;
};

// Ein Zustand spielt einen Clip oder mischt über parameter zwischen den Punkten von blend.
struct GraphState {
    std::string name;
    std::string clip;
    std::string parameter;
    std::vector<BlendPoint> blend;
    f32 speed = 1.0f;
    bool loop = true;
    std::vector<GraphEvent> events;

    friend bool operator==(const GraphState&, const GraphState&) = default;
};

enum class ConditionOp : u8 { Greater, Less, True, False, Trigger };

struct GraphCondition {
    std::string parameter;
    ConditionOp op = ConditionOp::True;
    f32 value = 0.0f;

    friend bool operator==(const GraphCondition&, const GraphCondition&) = default;
};

struct GraphTransition {
    std::string from;
    std::string to;
    std::vector<GraphCondition> conditions;
    // Dauer der Überblendung in Sekunden.
    f32 duration = 0.2f;
    // Erst, wenn der Zustand einmal durchgelaufen ist (Ende bzw. Ende einer Schleife).
    bool after_end = false;

    friend bool operator==(const GraphTransition&, const GraphTransition&) = default;
};

struct AnimGraph {
    // Modell mit Skelett und Clips.
    u64 model = 0;
    // Erster Zustand; leer: der erste in states.
    std::string initial;
    std::vector<GraphParameter> parameters;
    std::vector<GraphState> states;
    std::vector<GraphTransition> transitions;

    friend bool operator==(const AnimGraph&, const AnimGraph&) = default;
};

[[nodiscard]] asset::Result<AnimGraph> parse_anim_graph(std::string_view text);
[[nodiscard]] std::string write_anim_graph(const AnimGraph& graph);

// Lader für AnimGraph im Ressourcen-Manager (Hot Reload wie jedes Asset).
void register_graph_loader(asset::ResourceManager& resources);

} // namespace ngine::animation
