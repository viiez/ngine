module;

#include <algorithm>
#include <cmath>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.animation;

import ngine.core;
import ngine.asset;

namespace ngine::animation {

asset::Result<StateMachine> StateMachine::create(const AnimGraph& graph, const Rig& rig,
                                                 std::vector<std::string>* warnings) {
    const auto warn = [&](std::string message) {
        if (warnings != nullptr) {
            warnings->push_back(std::move(message));
        }
    };
    if (graph.states.empty()) {
        return std::unexpected(std::string{"Automat ohne Zustände"});
    }
    StateMachine machine;
    for (const GraphParameter& parameter : graph.parameters) {
        machine.parameters_.push_back({parameter.name, parameter.type, parameter.value});
    }
    for (const asset::Animation& clip : rig.clips) {
        machine.clip_durations_.push_back(clip.duration);
    }
    const auto clip = [&](std::string_view name, std::string_view state) -> std::optional<u32> {
        if (name.empty()) {
            return std::nullopt;
        }
        const auto found = rig.find_clip(name);
        if (!found) {
            warn(std::format("Zustand {}: Clip {} fehlt im Modell", state, name));
        }
        return found;
    };
    for (const GraphState& source : graph.states) {
        State state;
        state.name = source.name;
        state.clip = clip(source.clip, source.name);
        state.speed = source.speed;
        state.loop = source.loop;
        if (!source.parameter.empty()) {
            state.parameter = machine.find_parameter(source.parameter);
            if (!state.parameter) {
                warn(std::format("Zustand {}: Parameter {} fehlt", source.name, source.parameter));
            }
        }
        for (const BlendPoint& point : source.blend) {
            state.blend.push_back({clip(point.clip, source.name), point.value});
        }
        std::ranges::sort(state.blend, {}, &Point::value);
        for (const GraphEvent& event : source.events) {
            state.events.push_back({std::clamp(event.time, 0.0f, 1.0f), event.name});
        }
        machine.states_.push_back(std::move(state));
    }
    const auto state_index = [&](std::string_view name) -> std::optional<u32> {
        for (u32 i = 0; i < machine.states_.size(); ++i) {
            if (machine.states_[i].name == name) {
                return i;
            }
        }
        return std::nullopt;
    };
    for (const GraphTransition& source : graph.transitions) {
        Transition transition;
        transition.duration = std::max(source.duration, 0.0f);
        transition.after_end = source.after_end;
        const auto to = state_index(source.to);
        bool ok = to.has_value();
        if (to) {
            transition.to = *to;
        }
        if (source.from != "*") {
            transition.from = state_index(source.from);
            ok = ok && transition.from.has_value();
        }
        for (const GraphCondition& condition : source.conditions) {
            const auto parameter = machine.find_parameter(condition.parameter);
            if (!parameter) {
                ok = false;
                break;
            }
            transition.conditions.push_back({*parameter, condition.op, condition.value});
        }
        if (!ok) {
            warn(std::format("Übergang {} → {}: Zustand oder Parameter fehlt", source.from,
                             source.to));
            continue;
        }
        machine.transitions_.push_back(std::move(transition));
    }
    if (!graph.initial.empty()) {
        if (const auto initial = state_index(graph.initial)) {
            machine.current_.state = *initial;
        } else {
            warn(std::format("Anfangszustand {} fehlt", graph.initial));
        }
    }
    return machine;
}

std::optional<u32> StateMachine::find_parameter(std::string_view name) const noexcept {
    for (u32 i = 0; i < parameters_.size(); ++i) {
        if (parameters_[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

bool StateMachine::set_float(std::string_view name, f32 value) noexcept {
    const auto index = find_parameter(name);
    if (!index) {
        return false;
    }
    parameters_[*index].value = value;
    return true;
}

bool StateMachine::set_bool(std::string_view name, bool value) noexcept {
    return set_float(name, value ? 1.0f : 0.0f);
}

bool StateMachine::set_trigger(std::string_view name) noexcept {
    return set_float(name, 1.0f);
}

std::optional<f32> StateMachine::parameter(std::string_view name) const noexcept {
    const auto index = find_parameter(name);
    return index ? std::optional<f32>{parameters_[*index].value} : std::nullopt;
}

std::string_view StateMachine::state() const noexcept {
    return states_.empty() ? std::string_view{} : std::string_view{states_[current_.state].name};
}

std::string_view StateMachine::previous_state() const noexcept {
    return previous_ ? std::string_view{states_[previous_->state].name} : std::string_view{};
}

f32 StateMachine::fade() const noexcept {
    return fade_duration_ > 0.0f ? std::min(fade_time_ / fade_duration_, 1.0f) : 1.0f;
}

// Blend: die beiden Punkte um den Wert des Parameters und der Anteil des zweiten.
namespace {

struct Neighbors {
    usize a = 0;
    usize b = 0;
    f32 weight = 0.0f;
};

template <typename Points>
Neighbors neighbors(const Points& points, f32 value) noexcept {
    if (points.size() < 2 || value <= points.front().value) {
        return {};
    }
    if (value >= points.back().value) {
        return {points.size() - 1, points.size() - 1, 0.0f};
    }
    usize b = 1;
    while (b + 1 < points.size() && points[b].value < value) {
        ++b;
    }
    const f32 span = points[b].value - points[b - 1].value;
    return {b - 1, b, span > 0.0f ? (value - points[b - 1].value) / span : 0.0f};
}

} // namespace

f32 StateMachine::duration(const Rig* /*rig*/, const State& state) const noexcept {
    const auto clip_duration = [&](const std::optional<u32>& clip) {
        return clip && *clip < clip_durations_.size() ? clip_durations_[*clip] : 0.0f;
    };
    f32 seconds = 0.0f;
    if (!state.blend.empty()) {
        const f32 value = state.parameter ? parameters_[*state.parameter].value : 0.0f;
        const Neighbors n = neighbors(state.blend, value);
        // Gleiche normierte Zeit für beide Clips: Dauer dazwischen gemischt.
        seconds = clip_duration(state.blend[n.a].clip) * (1.0f - n.weight) +
                  clip_duration(state.blend[n.b].clip) * n.weight;
    } else {
        seconds = clip_duration(state.clip);
    }
    return std::max(seconds, 1e-3f);
}

bool StateMachine::met(const Transition& transition) const noexcept {
    if (transition.from ? *transition.from != current_.state : transition.to == current_.state) {
        return false;
    }
    if (transition.after_end && !current_.ended) {
        return false;
    }
    for (const Condition& condition : transition.conditions) {
        const f32 value = parameters_[condition.parameter].value;
        switch (condition.op) {
        case ConditionOp::Greater:
            if (!(value > condition.value)) {
                return false;
            }
            break;
        case ConditionOp::Less:
            if (!(value < condition.value)) {
                return false;
            }
            break;
        case ConditionOp::True:
        case ConditionOp::Trigger:
            if (value == 0.0f) {
                return false;
            }
            break;
        case ConditionOp::False:
            if (value != 0.0f) {
                return false;
            }
            break;
        }
    }
    return true;
}

void StateMachine::start(const Transition& transition) {
    for (const Condition& condition : transition.conditions) {
        if (condition.op == ConditionOp::Trigger) {
            parameters_[condition.parameter].value = 0.0f;
        }
    }
    if (transition.duration <= 0.0f) {
        previous_.reset();
        frozen_ = false;
        fade_duration_ = 0.0f;
    } else if (fade() < 1.0f && !last_pose_.empty()) {
        // Mitten in einer Überblendung: von der zuletzt gezeigten Pose aus, damit nichts springt.
        frozen_pose_ = last_pose_;
        frozen_ = true;
        previous_.reset();
    } else {
        previous_ = current_;
        frozen_ = false;
    }
    fade_time_ = 0.0f;
    fade_duration_ = transition.duration;
    current_ = Playing{.state = transition.to, .phase = 0.0f, .ended = false};
}

void StateMachine::advance(Playing& playing, f32 dt, std::vector<const std::string*>* events) {
    const State& state = states_[playing.state];
    const f32 before = playing.phase;
    f32 after = before + dt * state.speed / duration(nullptr, state);
    playing.ended = false;
    bool wrapped = false;
    if (after >= 1.0f) {
        playing.ended = true;
        if (state.loop) {
            after = std::fmod(after, 1.0f);
            wrapped = true;
        } else {
            after = 1.0f;
        }
    }
    if (events != nullptr && dt > 0.0f) {
        for (const Event& event : state.events) {
            const bool hit = wrapped ? event.time >= before || event.time < after
                                     : event.time >= before && event.time < after;
            if (hit) {
                events->push_back(&event.name);
            }
        }
    }
    playing.phase = after;
}

void StateMachine::update(f32 dt, std::vector<const std::string*>* events) {
    if (states_.empty()) {
        return;
    }
    // Höchstens ein Übergang pro Bild, in der Reihenfolge der Datei.
    for (const Transition& transition : transitions_) {
        if (met(transition)) {
            start(transition);
            break;
        }
    }
    if (previous_) {
        advance(*previous_, dt, nullptr);
    }
    advance(current_, dt, events);
    if (fade_duration_ > 0.0f) {
        fade_time_ += dt;
        if (fade_time_ >= fade_duration_) {
            previous_.reset();
            frozen_ = false;
            fade_duration_ = 0.0f;
            fade_time_ = 0.0f;
        }
    }
}

void StateMachine::evaluate_state(const Rig& rig, const Playing& playing, Pose& out) {
    const State& state = states_[playing.state];
    const auto clip_time = [&](const std::optional<u32>& clip) {
        return clip && *clip < clip_durations_.size() ? playing.phase * clip_durations_[*clip]
                                                      : 0.0f;
    };
    if (state.blend.empty()) {
        sample(rig, state.clip.value_or(~0u), clip_time(state.clip), out);
        return;
    }
    const f32 value = state.parameter ? parameters_[*state.parameter].value : 0.0f;
    const Neighbors n = neighbors(state.blend, value);
    const Point& a = state.blend[n.a];
    sample(rig, a.clip.value_or(~0u), clip_time(a.clip), out);
    if (n.weight > 0.0f && n.b != n.a) {
        const Point& b = state.blend[n.b];
        sample(rig, b.clip.value_or(~0u), clip_time(b.clip), scratch_b_);
        blend(out, scratch_b_, n.weight, out);
    }
}

void StateMachine::evaluate(const Rig& rig, Pose& out) {
    if (states_.empty()) {
        out = rig.rest;
        return;
    }
    evaluate_state(rig, current_, out);
    if (fade_duration_ > 0.0f && (previous_ || frozen_)) {
        if (previous_) {
            evaluate_state(rig, *previous_, scratch_a_);
        } else {
            scratch_a_ = frozen_pose_;
        }
        // Weicher Verlauf (smoothstep), stetig an beiden Enden.
        const f32 t = fade();
        const f32 weight = t * t * (3.0f - 2.0f * t);
        blend(scratch_a_, out, weight, out);
    }
    last_pose_ = out;
}

} // namespace ngine::animation
