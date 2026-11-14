module;

#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

module ngine.input;

import ngine.core;
import ngine.filesystem;
import ngine.platform;
import ngine.reflect;
import ngine.serialize;

namespace ngine::input {

namespace {

using Device = ActionMap::Device;
using Source = ActionMap::Source;

// Namen der Tasten in der Reihenfolge von platform::Key, ab A (Unknown hat keinen Namen).
constexpr std::array key_names{
    "A",         "B",           "C",
    "D",         "E",           "F",
    "G",         "H",           "I",
    "J",         "K",           "L",
    "M",         "N",           "O",
    "P",         "Q",           "R",
    "S",         "T",           "U",
    "V",         "W",           "X",
    "Y",         "Z",           "0",
    "1",         "2",           "3",
    "4",         "5",           "6",
    "7",         "8",           "9",
    "F1",        "F2",          "F3",
    "F4",        "F5",          "F6",
    "F7",        "F8",          "F9",
    "F10",       "F11",         "F12",
    "Escape",    "Enter",       "Tab",
    "Backspace", "Insert",      "Delete",
    "Home",      "End",         "PageUp",
    "PageDown",  "Left",        "Right",
    "Up",        "Down",        "Space",
    "Shift",     "Ctrl",        "Alt",
    "Super",     "LeftBracket", "RightBracket",
    "Comma",     "Period",      "Minus",
    "Equals",
};
static_assert(key_names.size() + 1 == static_cast<usize>(platform::Key::Count),
              "Jede Taste braucht einen Namen");

constexpr std::array mouse_button_names{"Left", "Middle", "Right"};
constexpr std::array mouse_axis_names{"X", "Y", "Wheel", "WheelX"};
constexpr std::array pad_button_names{
    "South",         "East",   "West",      "North",      "Back",
    "Guide",         "Start",  "LeftStick", "RightStick", "LeftShoulder",
    "RightShoulder", "DpadUp", "DpadDown",  "DpadLeft",   "DpadRight",
};
static_assert(pad_button_names.size() == static_cast<usize>(platform::GamepadButton::Count));
constexpr std::array pad_axis_names{"LeftX",  "LeftY",       "RightX",
                                    "RightY", "LeftTrigger", "RightTrigger"};
static_assert(pad_axis_names.size() == static_cast<usize>(platform::GamepadAxis::Count));

template <usize N>
std::optional<u8> index_of(const std::array<const char*, N>& names, std::string_view name) {
    for (usize i = 0; i < N; ++i) {
        if (name == names[i]) {
            return static_cast<u8>(i);
        }
    }
    return std::nullopt;
}

std::optional<Source> parse_source(std::string_view text) {
    const auto dot = text.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view device = text.substr(0, dot);
    const std::string_view name = text.substr(dot + 1);
    if (device == "Key") {
        if (const auto i = index_of(key_names, name)) {
            // +1: Key::Unknown steht vorn.
            return Source{Device::Key, static_cast<u8>(*i + 1)};
        }
    } else if (device == "Mouse") {
        if (const auto i = index_of(mouse_button_names, name)) {
            return Source{Device::MouseButton, *i};
        }
        if (const auto i = index_of(mouse_axis_names, name)) {
            return Source{Device::MouseAxis, *i};
        }
    } else if (device == "Gamepad") {
        if (const auto i = index_of(pad_button_names, name)) {
            return Source{Device::GamepadButton, *i};
        }
        if (const auto i = index_of(pad_axis_names, name)) {
            return Source{Device::GamepadAxis, *i};
        }
    }
    return std::nullopt;
}

f32 apply_dead_zone(f32 value, f32 dead_zone) noexcept {
    const f32 magnitude = std::abs(value);
    if (magnitude <= dead_zone || dead_zone >= 1.0f) {
        return 0.0f;
    }
    const f32 scaled = std::min((magnitude - dead_zone) / (1.0f - dead_zone), 1.0f);
    return value < 0.0f ? -scaled : scaled;
}

const ActionValue empty_value{};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    register_types(types);
    return types;
}

} // namespace

void register_types(reflect::TypeRegistry& types) {
    if (types.find<ActionKind>() == nullptr) {
        types.add<ActionKind>("ActionKind")
            .value("Button", ActionKind::Button)
            .value("Axis", ActionKind::Axis)
            .value("Axis2", ActionKind::Axis2);
    }
    if (types.find<Component>() == nullptr) {
        types.add<Component>("InputComponent").value("X", Component::X).value("Y", Component::Y);
    }
    if (types.find<Binding>() == nullptr) {
        types.add<Binding>("InputBinding")
            .field("source", &Binding::source)
            .field("component", &Binding::component)
            .field("scale", &Binding::scale)
            .field("per_second", &Binding::per_second);
    }
    if (types.find<Action>() == nullptr) {
        // Die Liste der Bindings kennt die Reflection nicht (keine Arrays); ActionMap::parse()
        // liest sie selbst und jedes Element über die Reflection.
        types.add<Action>("InputAction")
            .field("name", &Action::name)
            .field("kind", &Action::kind)
            .field("dead_zone", &Action::dead_zone)
            .range(0.0, 1.0)
            .field("sensitivity", &Action::sensitivity)
            .field("limit", &Action::limit);
    }
}

std::vector<std::string> source_names() {
    std::vector<std::string> names;
    for (const char* name : key_names) {
        names.push_back(std::format("Key.{}", name));
    }
    for (const char* name : mouse_button_names) {
        names.push_back(std::format("Mouse.{}", name));
    }
    for (const char* name : mouse_axis_names) {
        names.push_back(std::format("Mouse.{}", name));
    }
    for (const char* name : pad_button_names) {
        names.push_back(std::format("Gamepad.{}", name));
    }
    for (const char* name : pad_axis_names) {
        names.push_back(std::format("Gamepad.{}", name));
    }
    return names;
}

// --- ActionMap ------------------------------------------------------------------------------

Result<void> ActionMap::add(Action action) {
    if (action.name.empty()) {
        return std::unexpected(std::string{"Aktion ohne Namen"});
    }
    std::vector<Source> sources;
    for (const Binding& binding : action.bindings) {
        const auto source = parse_source(binding.source);
        if (!source) {
            return std::unexpected(
                std::format("Aktion {}: unbekannte Quelle \"{}\"", action.name, binding.source));
        }
        sources.push_back(*source);
    }
    if (const auto existing = find(action.name)) {
        actions_[*existing] = std::move(action);
        sources_[*existing] = std::move(sources);
    } else {
        actions_.push_back(std::move(action));
        sources_.push_back(std::move(sources));
    }
    return {};
}

std::optional<usize> ActionMap::find(std::string_view name) const noexcept {
    for (usize i = 0; i < actions_.size(); ++i) {
        if (actions_[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

Result<ActionMap> ActionMap::parse(std::string_view text) {
    auto root = json::parse(text);
    if (!root) {
        return std::unexpected(
            std::format("Zeile {}: {}", root.error().line, root.error().message));
    }
    const json::Value* list = root->find("actions");
    if (list == nullptr || list->array() == nullptr) {
        return std::unexpected(std::string{"Schlüssel \"actions\" mit einer Liste fehlt"});
    }
    const reflect::TypeRegistry types = make_types();
    ActionMap map;
    for (const json::Value& entry : *list->array()) {
        Action action;
        if (auto read = serialize::from_json(entry, action, types); !read) {
            return std::unexpected(read.error());
        }
        if (const json::Value* bindings = entry.find("bindings")) {
            if (bindings->array() == nullptr) {
                return std::unexpected(
                    std::format("Aktion {}: \"bindings\" muss eine Liste sein", action.name));
            }
            for (const json::Value& item : *bindings->array()) {
                Binding binding;
                if (auto read = serialize::from_json(item, binding, types); !read) {
                    return std::unexpected(std::format("Aktion {}: {}", action.name, read.error()));
                }
                action.bindings.push_back(std::move(binding));
            }
        }
        if (auto added = map.add(std::move(action)); !added) {
            return std::unexpected(added.error());
        }
    }
    return map;
}

Result<ActionMap> ActionMap::load_file(std::string_view path) {
    auto text = filesystem::read_text(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto map = parse(*text);
    if (!map) {
        return std::unexpected(std::format("{}: {}", path, map.error()));
    }
    return map;
}

ActionMap ActionMap::standard() {
    const auto key = [](std::string_view name, Component component = Component::X,
                        f32 scale = 1.0f) {
        return Binding{.source = std::string{name}, .component = component, .scale = scale};
    };
    const auto rate = [](std::string_view name, Component component, f32 scale) {
        return Binding{.source = std::string{name},
                       .component = component,
                       .scale = scale,
                       .per_second = true};
    };
    using enum Component;
    ActionMap map;
    const std::array actions{
        Action{.name = "move",
               .kind = ActionKind::Axis2,
               .limit = 1.0f,
               .bindings = {key("Key.W", Y), key("Key.S", Y, -1.0f), key("Key.D"),
                            key("Key.A", X, -1.0f), key("Gamepad.LeftX"), key("Gamepad.LeftY", Y)}},
        Action{.name = "look",
               .kind = ActionKind::Axis2,
               .bindings = {key("Mouse.X"), key("Mouse.Y", Y), rate("Gamepad.RightX", X, 900.0f),
                            rate("Gamepad.RightY", Y, -900.0f)}},
        Action{.name = "up_down",
               .kind = ActionKind::Axis,
               .limit = 1.0f,
               .bindings = {key("Key.E"), key("Key.Q", X, -1.0f), key("Gamepad.RightShoulder"),
                            key("Gamepad.LeftShoulder", X, -1.0f)}},
        Action{
            .name = "fast",
            .kind = ActionKind::Button,
            .bindings = {key("Key.Shift"), key("Gamepad.RightTrigger"), key("Gamepad.LeftStick")}},
        Action{.name = "jump",
               .kind = ActionKind::Button,
               .bindings = {key("Key.Space"), key("Gamepad.South")}},
        // Werfen auf dem Spielplatz (R4.2).
        Action{.name = "throw",
               .kind = ActionKind::Button,
               .bindings = {key("Mouse.Left"), key("Gamepad.West")}},
        // Spielfigur (R4.3): ducken, Kamera umschalten, graben.
        Action{.name = "crouch",
               .kind = ActionKind::Button,
               .bindings = {key("Key.Ctrl"), key("Gamepad.East")}},
        Action{.name = "camera",
               .kind = ActionKind::Button,
               .bindings = {key("Key.V"), key("Gamepad.RightStick")}},
        Action{.name = "dig",
               .kind = ActionKind::Button,
               .bindings = {key("Mouse.Right"), key("Gamepad.LeftTrigger")}},
    };
    for (const Action& action : actions) {
        const auto added = map.add(action);
        verify(added.has_value(), "Eingebaute Belegung ist fehlerhaft");
    }
    return map;
}

// --- ActionState ----------------------------------------------------------------------------

ActionState::ActionState(ActionMap map) {
    set_map(std::move(map));
}

void ActionState::set_map(ActionMap map) {
    map_ = std::move(map);
    values_.assign(map_.actions().size(), ActionValue{});
    fixed_.assign(map_.actions().size(), ActionValue{});
}

ActionState::Gamepad* ActionState::gamepad(u32 id) noexcept {
    for (Gamepad& pad : gamepads_) {
        if (pad.id == id) {
            return &pad;
        }
    }
    return nullptr;
}

void ActionState::begin_frame() {
    keys_pressed_.reset();
    mouse_pressed_.reset();
    pad_pressed_.reset();
    mouse_axes_.fill(0.0f);
}

void ActionState::handle(const platform::Event& event) {
    namespace ev = platform::event;
    if (const auto* key = std::get_if<ev::KeyPressed>(&event)) {
        keys_.set(static_cast<usize>(key->key));
        keys_pressed_.set(static_cast<usize>(key->key));
    } else if (const auto* released = std::get_if<ev::KeyReleased>(&event)) {
        keys_.reset(static_cast<usize>(released->key));
    } else if (const auto* button = std::get_if<ev::MouseButtonPressed>(&event)) {
        mouse_.set(static_cast<usize>(button->button));
        mouse_pressed_.set(static_cast<usize>(button->button));
    } else if (const auto* button_up = std::get_if<ev::MouseButtonReleased>(&event)) {
        mouse_.reset(static_cast<usize>(button_up->button));
    } else if (const auto* moved = std::get_if<ev::MouseMoved>(&event)) {
        mouse_axes_[0] += moved->dx;
        mouse_axes_[1] += moved->dy;
    } else if (const auto* wheel = std::get_if<ev::MouseWheel>(&event)) {
        mouse_axes_[2] += wheel->delta;
        mouse_axes_[3] += wheel->delta_x;
    } else if (const auto* added = std::get_if<ev::GamepadAdded>(&event)) {
        if (gamepad(added->gamepad) == nullptr) {
            gamepads_.push_back(Gamepad{.id = added->gamepad});
        }
    } else if (const auto* removed = std::get_if<ev::GamepadRemoved>(&event)) {
        std::erase_if(gamepads_, [&](const Gamepad& pad) { return pad.id == removed->gamepad; });
    } else if (const auto* pad_button = std::get_if<ev::GamepadButtonPressed>(&event)) {
        if (Gamepad* pad = gamepad(pad_button->gamepad)) {
            pad->buttons.set(static_cast<usize>(pad_button->button));
            pad_pressed_.set(static_cast<usize>(pad_button->button));
        }
    } else if (const auto* pad_up = std::get_if<ev::GamepadButtonReleased>(&event)) {
        if (Gamepad* pad = gamepad(pad_up->gamepad)) {
            pad->buttons.reset(static_cast<usize>(pad_up->button));
        }
    } else if (const auto* axis = std::get_if<ev::GamepadAxisMoved>(&event)) {
        if (Gamepad* pad = gamepad(axis->gamepad)) {
            pad->axes[static_cast<usize>(axis->axis)] = axis->value;
        }
    } else if (const auto* focus = std::get_if<ev::FocusChanged>(&event)) {
        if (!focus->focused) {
            release_all();
        }
    }
}

void ActionState::release_all() {
    keys_.reset();
    mouse_.reset();
    mouse_axes_.fill(0.0f);
    for (Gamepad& pad : gamepads_) {
        pad.buttons.reset();
        pad.axes.fill(0.0f);
    }
}

f32 ActionState::source_value(const Source& source, const Action& action) const noexcept {
    switch (source.device) {
    case Device::Key: return keys_.test(source.code) ? 1.0f : 0.0f;
    case Device::MouseButton: return mouse_.test(source.code) ? 1.0f : 0.0f;
    case Device::MouseAxis: return mouse_axes_[source.code];
    case Device::GamepadButton:
        for (const Gamepad& pad : gamepads_) {
            if (pad.buttons.test(source.code)) {
                return 1.0f;
            }
        }
        return 0.0f;
    case Device::GamepadAxis: {
        // Bei mehreren Gamepads zählt der stärkste Ausschlag.
        f32 strongest = 0.0f;
        for (const Gamepad& pad : gamepads_) {
            const f32 value = apply_dead_zone(pad.axes[source.code], action.dead_zone);
            if (std::abs(value) > std::abs(strongest)) {
                strongest = value;
            }
        }
        return strongest;
    }
    }
    return 0.0f;
}

bool ActionState::source_pressed(const Source& source) const noexcept {
    switch (source.device) {
    case Device::Key: return keys_pressed_.test(source.code);
    case Device::MouseButton: return mouse_pressed_.test(source.code);
    case Device::GamepadButton: return pad_pressed_.test(source.code);
    default: return false;
    }
}

void ActionState::end_frame(f32 dt) {
    const auto& actions = map_.actions();
    for (usize i = 0; i < actions.size(); ++i) {
        const Action& action = actions[i];
        const auto& sources = map_.sources(i);
        f32 x = 0.0f;
        f32 y = 0.0f;
        bool had_press = false;
        for (usize s = 0; s < sources.size(); ++s) {
            const Binding& binding = action.bindings[s];
            f32 value = source_value(sources[s], action) * binding.scale;
            if (binding.per_second) {
                value *= dt;
            }
            had_press = had_press || source_pressed(sources[s]);
            if (action.kind == ActionKind::Button) {
                x = std::max(x, value);
            } else if (action.kind == ActionKind::Axis2 && binding.component == Component::Y) {
                y += value;
            } else {
                x += value;
            }
        }
        if (action.kind != ActionKind::Button) {
            x *= action.sensitivity;
            y *= action.sensitivity;
            const f32 length = std::sqrt(x * x + y * y);
            if (action.limit > 0.0f && length > action.limit) {
                x *= action.limit / length;
                y *= action.limit / length;
            }
        }

        ActionValue& value = values_[i];
        const bool was_down = value.down;
        const bool down = action.kind == ActionKind::Button ? x >= 0.5f : (x != 0.0f || y != 0.0f);
        value.x = x;
        value.y = y;
        value.down = down;
        if (action.kind == ActionKind::Button) {
            // Ein kurzer Druck innerhalb eines Bildes zählt als gedrückt und losgelassen.
            const bool tap = had_press && !down && !was_down;
            value.pressed = !was_down && (down || had_press);
            value.released = (was_down && !down) || tap;
        } else {
            value.pressed = !was_down && down;
            value.released = was_down && !down;
        }

        ActionValue& fixed = fixed_[i];
        fixed.x = value.x;
        fixed.y = value.y;
        fixed.pressed = fixed.pressed || value.pressed;
        fixed.released = fixed.released || value.released;
        fixed.down = value.down || fixed.pressed;
    }
}

void ActionState::end_fixed_step() {
    for (usize i = 0; i < fixed_.size(); ++i) {
        fixed_[i].pressed = false;
        fixed_[i].released = false;
        fixed_[i].down = values_[i].down;
    }
}

const ActionValue& ActionState::value(std::string_view action) const noexcept {
    const auto index = map_.find(action);
    return index ? values_[*index] : empty_value;
}

const ActionValue& ActionState::fixed(std::string_view action) const noexcept {
    const auto index = map_.find(action);
    return index ? fixed_[*index] : empty_value;
}

} // namespace ngine::input
