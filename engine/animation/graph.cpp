module;

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
import ngine.filesystem;
import ngine.serialize;

namespace ngine::animation {

namespace {

std::string_view to_string(ParameterType type) noexcept {
    switch (type) {
    case ParameterType::Float: return "float";
    case ParameterType::Bool: return "bool";
    case ParameterType::Trigger: return "trigger";
    }
    return "float";
}

std::string_view to_string(ConditionOp op) noexcept {
    switch (op) {
    case ConditionOp::Greater: return "gt";
    case ConditionOp::Less: return "lt";
    case ConditionOp::True: return "true";
    case ConditionOp::False: return "false";
    case ConditionOp::Trigger: return "trigger";
    }
    return "true";
}

std::string text_of(const json::Value& object, std::string_view key) {
    const json::Value* value = object.find(key);
    return value != nullptr && value->string() != nullptr ? *value->string() : std::string{};
}

f32 number_of(const json::Value& object, std::string_view key, f32 fallback) {
    const json::Value* value = object.find(key);
    const auto number = value != nullptr ? value->to_f64() : std::nullopt;
    return number ? static_cast<f32>(*number) : fallback;
}

bool flag_of(const json::Value& object, std::string_view key, bool fallback) {
    const json::Value* value = object.find(key);
    const auto flag = value != nullptr ? value->to_bool() : std::nullopt;
    return flag ? *flag : fallback;
}

const json::Array* list_of(const json::Value& object, std::string_view key) {
    const json::Value* value = object.find(key);
    return value != nullptr ? value->array() : nullptr;
}

} // namespace

asset::Result<AnimGraph> parse_anim_graph(std::string_view text) {
    auto root = json::parse(text);
    if (!root) {
        return std::unexpected(
            std::format("Zeile {}: {}", root.error().line, root.error().message));
    }
    if (text_of(*root, "format") != anim_graph_format) {
        return std::unexpected(std::format("Kein Format \"{}\"", anim_graph_format));
    }
    AnimGraph graph;
    if (const json::Value* model = root->find("model")) {
        graph.model = model->to_u64().value_or(0);
    }
    graph.initial = text_of(*root, "initial");

    if (const json::Array* parameters = list_of(*root, "parameters")) {
        for (const json::Value& entry : *parameters) {
            GraphParameter parameter;
            parameter.name = text_of(entry, "name");
            const std::string type = text_of(entry, "type");
            if (type == "bool") {
                parameter.type = ParameterType::Bool;
                parameter.value = flag_of(entry, "value", false) ? 1.0f : 0.0f;
            } else if (type == "trigger") {
                parameter.type = ParameterType::Trigger;
            } else if (type.empty() || type == "float") {
                parameter.value = number_of(entry, "value", 0.0f);
            } else {
                return std::unexpected(
                    std::format("Parameter {}: Art \"{}\" unbekannt", parameter.name, type));
            }
            if (parameter.name.empty()) {
                return std::unexpected(std::string{"Parameter ohne Namen"});
            }
            graph.parameters.push_back(std::move(parameter));
        }
    }

    const json::Array* states = list_of(*root, "states");
    if (states == nullptr || states->empty()) {
        return std::unexpected(
            std::string{"Schlüssel \"states\" mit mindestens einem Zustand fehlt"});
    }
    for (const json::Value& entry : *states) {
        GraphState state;
        state.name = text_of(entry, "name");
        state.clip = text_of(entry, "clip");
        state.parameter = text_of(entry, "parameter");
        state.speed = number_of(entry, "speed", 1.0f);
        state.loop = flag_of(entry, "loop", true);
        if (const json::Array* blend = list_of(entry, "blend")) {
            for (const json::Value& point : *blend) {
                state.blend.push_back({text_of(point, "clip"), number_of(point, "value", 0.0f)});
            }
        }
        if (const json::Array* events = list_of(entry, "events")) {
            for (const json::Value& event : *events) {
                state.events.push_back({number_of(event, "time", 0.0f), text_of(event, "name")});
            }
        }
        if (state.name.empty()) {
            return std::unexpected(std::string{"Zustand ohne Namen"});
        }
        if (!state.blend.empty() && state.parameter.empty()) {
            return std::unexpected(std::format("Zustand {}: Blend ohne \"parameter\"", state.name));
        }
        graph.states.push_back(std::move(state));
    }

    if (const json::Array* transitions = list_of(*root, "transitions")) {
        for (const json::Value& entry : *transitions) {
            GraphTransition transition;
            transition.from = text_of(entry, "from");
            transition.to = text_of(entry, "to");
            transition.duration = number_of(entry, "duration", 0.2f);
            transition.after_end = flag_of(entry, "after_end", false);
            if (const json::Array* conditions = list_of(entry, "conditions")) {
                for (const json::Value& item : *conditions) {
                    GraphCondition condition;
                    condition.parameter = text_of(item, "parameter");
                    condition.value = number_of(item, "value", 0.0f);
                    const std::string op = text_of(item, "op");
                    if (op == "gt") {
                        condition.op = ConditionOp::Greater;
                    } else if (op == "lt") {
                        condition.op = ConditionOp::Less;
                    } else if (op == "true" || op.empty()) {
                        condition.op = ConditionOp::True;
                    } else if (op == "false") {
                        condition.op = ConditionOp::False;
                    } else if (op == "trigger") {
                        condition.op = ConditionOp::Trigger;
                    } else {
                        return std::unexpected(std::format("Bedingung \"{}\" unbekannt", op));
                    }
                    transition.conditions.push_back(std::move(condition));
                }
            }
            if (transition.from.empty() || transition.to.empty()) {
                return std::unexpected(std::string{"Übergang ohne \"from\" oder \"to\""});
            }
            graph.transitions.push_back(std::move(transition));
        }
    }
    return graph;
}

std::string write_anim_graph(const AnimGraph& graph) {
    json::Value root;
    root["format"] = anim_graph_format;
    root["version"] = anim_graph_version;
    root["model"] = graph.model;
    root["initial"] = graph.initial;
    json::Array parameters;
    for (const GraphParameter& parameter : graph.parameters) {
        json::Value entry;
        entry["name"] = parameter.name;
        entry["type"] = to_string(parameter.type);
        if (parameter.type == ParameterType::Bool) {
            entry["value"] = parameter.value != 0.0f;
        } else if (parameter.type == ParameterType::Float) {
            entry["value"] = parameter.value;
        }
        parameters.push_back(std::move(entry));
    }
    root["parameters"] = json::Value{std::move(parameters)};
    json::Array states;
    for (const GraphState& state : graph.states) {
        json::Value entry;
        entry["name"] = state.name;
        if (!state.clip.empty()) {
            entry["clip"] = state.clip;
        }
        if (!state.parameter.empty()) {
            entry["parameter"] = state.parameter;
        }
        if (!state.blend.empty()) {
            json::Array blend;
            for (const BlendPoint& point : state.blend) {
                json::Value item;
                item["clip"] = point.clip;
                item["value"] = point.value;
                blend.push_back(std::move(item));
            }
            entry["blend"] = json::Value{std::move(blend)};
        }
        entry["speed"] = state.speed;
        entry["loop"] = state.loop;
        if (!state.events.empty()) {
            json::Array events;
            for (const GraphEvent& event : state.events) {
                json::Value item;
                item["time"] = event.time;
                item["name"] = event.name;
                events.push_back(std::move(item));
            }
            entry["events"] = json::Value{std::move(events)};
        }
        states.push_back(std::move(entry));
    }
    root["states"] = json::Value{std::move(states)};
    json::Array transitions;
    for (const GraphTransition& transition : graph.transitions) {
        json::Value entry;
        entry["from"] = transition.from;
        entry["to"] = transition.to;
        entry["duration"] = transition.duration;
        entry["after_end"] = transition.after_end;
        json::Array conditions;
        for (const GraphCondition& condition : transition.conditions) {
            json::Value item;
            item["parameter"] = condition.parameter;
            item["op"] = to_string(condition.op);
            if (condition.op == ConditionOp::Greater || condition.op == ConditionOp::Less) {
                item["value"] = condition.value;
            }
            conditions.push_back(std::move(item));
        }
        entry["conditions"] = json::Value{std::move(conditions)};
        transitions.push_back(std::move(entry));
    }
    root["transitions"] = json::Value{std::move(transitions)};
    return json::write(root);
}

void register_graph_loader(asset::ResourceManager& resources) {
    resources.add_loader<AnimGraph, AnimGraph>(
        [](const asset::LoadContext& context) -> asset::Result<AnimGraph> {
            auto text = filesystem::read_text(context.path);
            if (!text) {
                return std::unexpected(std::move(text.error()));
            }
            auto graph = parse_anim_graph(*text);
            if (!graph) {
                return std::unexpected(std::format("{}: {}", context.path, graph.error()));
            }
            return std::move(*graph);
        },
        [](AnimGraph& graph) -> asset::Result<AnimGraph> { return std::move(graph); });
}

} // namespace ngine::animation
