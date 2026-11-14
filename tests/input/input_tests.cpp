#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>

import ngine.core;
import ngine.input;
import ngine.platform;

using namespace ngine;
using Catch::Matchers::WithinAbs;
namespace ev = platform::event;

namespace {

// Ein Bild mit den gegebenen Ereignissen.
template <typename... Events>
void frame(input::ActionState& state, f32 dt, Events&&... events) {
    state.begin_frame();
    (state.handle(platform::Event{std::forward<Events>(events)}), ...);
    state.end_frame(dt);
}

} // namespace

TEST_CASE("Standard map parses from the asset file", "[input]") {
    const auto loaded =
        input::ActionMap::load_file(NGINE_ASSET_SOURCE_DIR "/eingabe/standard.input");
    REQUIRE(loaded.has_value());
    const input::ActionMap standard = input::ActionMap::standard();
    REQUIRE(loaded->actions().size() == standard.actions().size());
    for (usize i = 0; i < standard.actions().size(); ++i) {
        const input::Action& a = loaded->actions()[i];
        const input::Action& b = standard.actions()[i];
        CHECK(a.name == b.name);
        CHECK(a.kind == b.kind);
        CHECK(a.limit == b.limit);
        REQUIRE(a.bindings.size() == b.bindings.size());
        for (usize j = 0; j < a.bindings.size(); ++j) {
            CHECK(a.bindings[j].source == b.bindings[j].source);
            CHECK(a.bindings[j].component == b.bindings[j].component);
            CHECK(a.bindings[j].scale == b.bindings[j].scale);
            CHECK(a.bindings[j].per_second == b.bindings[j].per_second);
        }
    }
}

TEST_CASE("Action map rejects unknown sources and bad files", "[input]") {
    CHECK_FALSE(input::ActionMap::parse("{}").has_value());
    CHECK_FALSE(input::ActionMap::parse("nicht json").has_value());
    const auto unknown = input::ActionMap::parse(
        R"({ "actions": [ { "name": "jump", "bindings": [ { "source": "Key.Kaffee" } ] } ] })");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().find("Key.Kaffee") != std::string::npos);
    // Jeder Name aus source_names() ist gültig.
    for (const std::string& name : input::source_names()) {
        input::ActionMap map;
        CHECK(map.add({.name = "test", .bindings = {{.source = name}}}).has_value());
    }
}

TEST_CASE("Jump comes from keyboard and gamepad", "[input]") {
    input::ActionState state{input::ActionMap::standard()};
    frame(state, 0.016f, ev::KeyPressed{platform::Key::Space});
    CHECK(state.pressed("jump"));
    CHECK(state.down("jump"));
    frame(state, 0.016f);
    CHECK_FALSE(state.pressed("jump"));
    CHECK(state.down("jump"));
    frame(state, 0.016f, ev::KeyReleased{platform::Key::Space});
    CHECK(state.released("jump"));
    CHECK_FALSE(state.down("jump"));

    // Gamepad: Tasten zählen erst, wenn es angesteckt ist.
    frame(state, 0.016f, ev::GamepadButtonPressed{7, platform::GamepadButton::South});
    CHECK_FALSE(state.down("jump"));
    frame(state, 0.016f, ev::GamepadAdded{7, "Testpad"},
          ev::GamepadButtonPressed{7, platform::GamepadButton::South});
    CHECK(state.gamepad_count() == 1);
    CHECK(state.pressed("jump"));
    // Abziehen lässt die Taste los.
    frame(state, 0.016f, ev::GamepadRemoved{7});
    CHECK(state.released("jump"));
    CHECK(state.gamepad_count() == 0);
}

TEST_CASE("Move combines WASD and stick with dead zone and limit", "[input]") {
    input::ActionState state{input::ActionMap::standard()};
    frame(state, 0.016f, ev::KeyPressed{platform::Key::W}, ev::KeyPressed{platform::Key::D});
    const input::ActionValue& move = state.value("move");
    // Diagonale auf Länge 1 begrenzt.
    CHECK_THAT(move.x, WithinAbs(0.70710678, 1e-5));
    CHECK_THAT(move.y, WithinAbs(0.70710678, 1e-5));
    frame(state, 0.016f, ev::KeyReleased{platform::Key::W}, ev::KeyReleased{platform::Key::D});
    CHECK_FALSE(state.down("move"));

    frame(state, 0.016f, ev::GamepadAdded{1, "Pad"},
          ev::GamepadAxisMoved{1, platform::GamepadAxis::LeftX, 0.1f},
          ev::GamepadAxisMoved{1, platform::GamepadAxis::LeftY, 0.6f});
    // 0,1 liegt in der toten Zone (0,2), 0,6 wird auf (0,6 - 0,2) / 0,8 = 0,5 skaliert.
    CHECK(state.value("move").x == 0.0f);
    CHECK_THAT(state.value("move").y, WithinAbs(0.5, 1e-5));
}

TEST_CASE("Look from mouse is per frame, from stick per second", "[input]") {
    input::ActionState state{input::ActionMap::standard()};
    frame(state, 0.01f, ev::MouseMoved{0.0f, 0.0f, 3.0f, -2.0f},
          ev::MouseMoved{0.0f, 0.0f, 1.0f, 0.0f});
    CHECK(state.value("look").x == 4.0f);
    CHECK(state.value("look").y == -2.0f);
    // Ohne Bewegung ist die Maus im nächsten Bild wieder bei 0.
    frame(state, 0.01f);
    CHECK_FALSE(state.down("look"));

    frame(state, 0.5f, ev::GamepadAdded{1, "Pad"},
          ev::GamepadAxisMoved{1, platform::GamepadAxis::RightX, 1.0f});
    CHECK_THAT(state.value("look").x, WithinAbs(450.0, 1e-3));
}

TEST_CASE("Fixed steps see short presses exactly once", "[input]") {
    input::ActionState state{input::ActionMap::standard()};

    // Niedrige Bildrate: Drücken und Loslassen im selben Bild, danach zwei feste Schritte. Der
    // erste sieht den Druck, der zweite nicht mehr.
    frame(state, 0.033f, ev::KeyPressed{platform::Key::Space},
          ev::KeyReleased{platform::Key::Space});
    CHECK(state.pressed("jump"));
    CHECK(state.released("jump"));
    CHECK_FALSE(state.down("jump"));
    CHECK(state.fixed("jump").pressed);
    CHECK(state.fixed("jump").down);
    state.end_fixed_step();
    CHECK_FALSE(state.fixed("jump").pressed);
    CHECK_FALSE(state.fixed("jump").down);

    // Hohe Bildrate: Der Druck kommt in einem Bild ohne festen Schritt. Er bleibt stehen, bis der
    // nächste Schritt ihn gesehen hat, und zählt dort genau einmal.
    frame(state, 0.004f, ev::KeyPressed{platform::Key::Space});
    frame(state, 0.004f);
    frame(state, 0.004f);
    CHECK_FALSE(state.pressed("jump"));
    CHECK(state.fixed("jump").pressed);
    state.end_fixed_step();
    CHECK_FALSE(state.fixed("jump").pressed);
    CHECK(state.fixed("jump").down);
}

TEST_CASE("Losing focus releases all keys", "[input]") {
    input::ActionState state{input::ActionMap::standard()};
    frame(state, 0.016f, ev::KeyPressed{platform::Key::Shift});
    CHECK(state.down("fast"));
    frame(state, 0.016f, ev::FocusChanged{false});
    CHECK_FALSE(state.down("fast"));
    CHECK(state.released("fast"));
}

TEST_CASE("New map keeps held keys", "[input]") {
    input::ActionState state{input::ActionMap::standard()};
    frame(state, 0.016f, ev::KeyPressed{platform::Key::J});
    auto map = input::ActionMap::parse(
        R"({ "actions": [ { "name": "jump", "kind": "Button",
                            "bindings": [ { "source": "Key.J" } ] } ] })");
    REQUIRE(map.has_value());
    state.set_map(std::move(*map));
    frame(state, 0.016f);
    CHECK(state.down("jump"));
    CHECK_FALSE(state.down("move"));
}
