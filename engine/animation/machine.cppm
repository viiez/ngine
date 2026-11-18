module;

#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module ngine.animation:machine;

import ngine.core;
import ngine.asset;
import :rig;
import :graph;

export namespace ngine::animation {

// Laufender Zustandsautomat eines Animators (ANIM-02): Parameter, aktiver Zustand mit normierter
// Zeit, Überblendung. Kennt nur Indizes; Namen löst create() gegen den Rig auf. Ohne Sperren und
// ohne Welt, damit das AnimationSystem viele Automaten parallel in Jobs rechnen kann.
class StateMachine {
public:
    StateMachine() = default;

    // Unbekannte Clips spielen die Ruhelage, unbekannte Parameter und Zustände in Übergängen
    // lassen den Übergang weg; warnings sammelt die Meldungen dazu.
    [[nodiscard]] static asset::Result<StateMachine>
    create(const AnimGraph& graph, const Rig& rig, std::vector<std::string>* warnings = nullptr);

    // Liefert false, wenn es den Parameter nicht gibt.
    bool set_float(std::string_view name, f32 value) noexcept;
    bool set_bool(std::string_view name, bool value) noexcept;
    bool set_trigger(std::string_view name) noexcept;
    [[nodiscard]] std::optional<f32> parameter(std::string_view name) const noexcept;
    [[nodiscard]] usize parameter_count() const noexcept {
        return parameters_.size();
    }
    [[nodiscard]] std::string_view parameter_name(usize index) const noexcept {
        return parameters_[index].name;
    }
    [[nodiscard]] ParameterType parameter_type(usize index) const noexcept {
        return parameters_[index].type;
    }
    [[nodiscard]] f32 parameter_value(usize index) const noexcept {
        return parameters_[index].value;
    }

    // Prüft Übergänge und rückt die Zeit um dt Sekunden vor. events bekommt die Namen der
    // Ereignisse, die dabei überschritten wurden (Zeiger in den Automaten, gültig bis zum nächsten
    // create()).
    void update(f32 dt, std::vector<const std::string*>* events = nullptr);

    // Pose aus aktivem Zustand und Überblendung. out wird auf die Knoten des Rigs gebracht; der
    // Rig muss der aus create() sein.
    void evaluate(const Rig& rig, Pose& out);

    [[nodiscard]] std::string_view state() const noexcept;
    // Zustand, aus dem gerade übergeblendet wird (leer ohne Überblendung oder bei einer
    // eingefrorenen Pose).
    [[nodiscard]] std::string_view previous_state() const noexcept;
    // Anteil der Überblendung (1: fertig).
    [[nodiscard]] f32 fade() const noexcept;
    // Normierte Zeit des aktiven Zustands (0 bis 1, bei Schleifen wiederholt).
    [[nodiscard]] f32 phase() const noexcept {
        return current_.phase;
    }
    [[nodiscard]] bool valid() const noexcept {
        return !states_.empty();
    }

private:
    struct Parameter {
        std::string name;
        ParameterType type = ParameterType::Float;
        f32 value = 0.0f;
    };
    struct Point {
        std::optional<u32> clip;
        f32 value = 0.0f;
    };
    struct Event {
        f32 time = 0.0f;
        std::string name;
    };
    struct State {
        std::string name;
        std::optional<u32> clip;
        std::optional<u32> parameter;
        std::vector<Point> blend;
        f32 speed = 1.0f;
        bool loop = true;
        std::vector<Event> events;
    };
    struct Condition {
        u32 parameter = 0;
        ConditionOp op = ConditionOp::True;
        f32 value = 0.0f;
    };
    struct Transition {
        // Ohne from gilt der Übergang für jeden Zustand außer to.
        std::optional<u32> from;
        u32 to = 0;
        std::vector<Condition> conditions;
        f32 duration = 0.2f;
        bool after_end = false;
    };
    // Ein laufender Zustand mit seiner normierten Zeit.
    struct Playing {
        u32 state = 0;
        f32 phase = 0.0f;
        // Ende erreicht (ohne Schleife) bzw. in diesem update() einmal durchgelaufen.
        bool ended = false;
    };

    [[nodiscard]] std::optional<u32> find_parameter(std::string_view name) const noexcept;
    [[nodiscard]] f32 duration(const Rig* rig, const State& state) const noexcept;
    [[nodiscard]] bool met(const Transition& transition) const noexcept;
    void start(const Transition& transition);
    void advance(Playing& playing, f32 dt, std::vector<const std::string*>* events);
    void evaluate_state(const Rig& rig, const Playing& playing, Pose& out);

    std::vector<Parameter> parameters_;
    std::vector<State> states_;
    std::vector<Transition> transitions_;
    // Dauer der Clips (Sekunden), Index wie im Rig.
    std::vector<f32> clip_durations_;
    Playing current_;
    std::optional<Playing> previous_;
    // Eingefrorene Pose, wenn ein Übergang mitten in einer Überblendung beginnt.
    bool frozen_ = false;
    Pose frozen_pose_;
    f32 fade_time_ = 0.0f;
    f32 fade_duration_ = 0.0f;
    Pose last_pose_;
    Pose scratch_a_;
    Pose scratch_b_;
};

} // namespace ngine::animation
