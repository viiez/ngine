#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

import ngine.core;
import ngine.editor_ui;

// Editor-UI ohne Fenster und GPU: Zeichenliste, Texturen, Eingaben, Konsole und Viewport-Größe.

namespace {

using namespace ngine;
namespace ui = editor_ui;

// Hält Texturen im Speicher statt auf der GPU.
class FakeHost final : public ui::TextureHost {
public:
    ui::TextureId create_texture(u32 width, u32 height, std::span<const u8> rgba) override {
        const ui::TextureId id = next++;
        live[id] = {width, height, rgba.size()};
        ++created;
        return id;
    }
    void destroy_texture(ui::TextureId texture) override {
        live.erase(texture);
        ++destroyed;
    }

    struct Info {
        u32 width = 0;
        u32 height = 0;
        usize bytes = 0;
    };
    std::map<ui::TextureId, Info> live;
    ui::TextureId next = 1;
    u32 created = 0;
    u32 destroyed = 0;
};

constexpr f32 width = 800.0f;
constexpr f32 height = 600.0f;
constexpr f32 dt = 1.0f / 60.0f;

} // namespace

TEST_CASE("Editor UI produces draw data and a font texture", "[editor_ui]") {
    FakeHost host;
    {
        ui::Context context{host};
        // Ein neues Fenster bleibt im ersten Frame unsichtbar, während ImGui seine Größe misst.
        const ui::UiDrawData* frame_data = nullptr;
        for (int frame = 0; frame < 2; ++frame) {
            context.begin_frame(width, height, dt);
            if (ui::begin_window("Test")) {
                ui::text("Hallo Editor");
                bool flag = true;
                ui::checkbox("Schalter", flag);
            }
            ui::end_window();
            frame_data = &context.end_frame();
        }
        const ui::UiDrawData& data = *frame_data;

        // Der Schriftatlas entsteht beim ersten Frame über den Host.
        REQUIRE(host.live.size() == 1);
        const auto& [font, info] = *host.live.begin();
        CHECK(info.bytes == usize{info.width} * info.height * 4);

        CHECK(data.width == width);
        CHECK(data.height == height);
        REQUIRE_FALSE(data.empty());
        for (const ui::UiCommand& command : data.commands) {
            CHECK(command.texture == font);
            CHECK(command.index_count > 0);
            CHECK(command.first_index + command.index_count <= data.indices.size());
            CHECK(command.clip.min_x >= 0.0f);
            CHECK(command.clip.max_x <= width);
            CHECK(command.clip.min_x < command.clip.max_x);
            CHECK(command.clip.min_y < command.clip.max_y);
        }
        for (const u32 index : data.indices) {
            CHECK(index < data.vertices.size());
        }
        CHECK(context.frame_count() == 2);
    }
    // Am Ende gibt der Kontext seine Texturen zurück.
    CHECK(host.live.empty());
    CHECK(host.destroyed == host.created);
}

TEST_CASE("Editor UI reports clicks from mouse input", "[editor_ui]") {
    FakeHost host;
    ui::Context context{host};
    ui::Point position;
    bool clicked = false;
    const auto frame = [&] {
        context.begin_frame(width, height, dt);
        ui::begin_window("Knopf");
        clicked = ui::button("Drücken") || clicked;
        position = ui::item_min();
        ui::end_window();
        (void)context.end_frame();
    };

    frame();
    frame();
    CHECK_FALSE(clicked);
    context.add_mouse_position(position.x + 4.0f, position.y + 4.0f);
    frame();
    CHECK(context.wants_mouse());
    context.add_mouse_button(ui::MouseButton::Left, true);
    frame();
    context.add_mouse_button(ui::MouseButton::Left, false);
    frame();
    CHECK(clicked);
}

TEST_CASE("Editor UI takes text input into a focused field", "[editor_ui]") {
    FakeHost host;
    ui::Context context{host};
    std::string value = "ab";
    ui::Point position;
    const auto frame = [&] {
        context.begin_frame(width, height, dt);
        ui::begin_window("Eingabe");
        ui::input_text("Feld", value);
        position = ui::item_min();
        ui::end_window();
        (void)context.end_frame();
    };

    frame();
    frame();
    context.add_mouse_position(position.x + 4.0f, position.y + 4.0f);
    frame();
    context.add_mouse_button(ui::MouseButton::Left, true);
    frame();
    context.add_mouse_button(ui::MouseButton::Left, false);
    frame();
    CHECK(context.wants_text_input());
    // Der Klick setzt die Schreibmarke an den Anfang; End schiebt sie ans Ende.
    context.add_key(ui::Key::End, true);
    frame();
    context.add_key(ui::Key::End, false);
    frame();
    context.add_text("cd");
    frame();
    // Ein Text, der länger als der alte Puffer ist, lässt den String wachsen.
    context.add_text("efghijklmnopqrstuvwxyz");
    frame();
    CHECK(value == "abcdefghijklmnopqrstuvwxyz");
    context.add_key(ui::Key::Backspace, true);
    frame();
    context.add_key(ui::Key::Backspace, false);
    frame();
    CHECK(value == "abcdefghijklmnopqrstuvwxy");
}

TEST_CASE("Editor UI shows images with their texture id", "[editor_ui]") {
    FakeHost host;
    ui::Context context{host};
    const ui::UiDrawData* frame_data = nullptr;
    for (int frame = 0; frame < 2; ++frame) {
        context.begin_frame(width, height, dt);
        ui::begin_window("Bild", nullptr, ui::WindowFlags::NoPadding);
        ui::image(42, {64.0f, 32.0f});
        ui::end_window();
        frame_data = &context.end_frame();
    }
    const ui::UiDrawData& data = *frame_data;

    bool found = false;
    for (const ui::UiCommand& command : data.commands) {
        found = found || command.texture == 42;
    }
    CHECK(found);
}

TEST_CASE("Editor UI docks windows into the default layout", "[editor_ui]") {
    FakeHost host;
    ui::Context context{host};
    const ui::DockLayout layout{.center = {"Mitte"}, .right = {"Rechts"}, .bottom = {"Unten"}};
    ui::Size center;
    ui::Size right;
    for (int i = 0; i < 3; ++i) {
        context.begin_frame(width, height, dt);
        ui::dockspace(layout);
        ui::begin_window("Mitte");
        center = ui::content_region_available();
        ui::end_window();
        ui::begin_window("Rechts");
        right = ui::content_region_available();
        ui::end_window();
        ui::begin_window("Unten");
        ui::end_window();
        (void)context.end_frame();
    }
    // Die Mitte ist größer als der rechte Streifen, beide zusammen kaum breiter als das Fenster.
    CHECK(center.width > right.width);
    CHECK(center.width + right.width <= width);
    CHECK(center.height < height);
}

TEST_CASE("Console filter checks level and text", "[editor_ui]") {
    const ui::ConsoleFilter all;
    const log::RingSink::Entry warn{log::Level::Warn, {}, "Textur fehlt: Stein.png"};
    CHECK(ui::matches(warn, all));

    ui::ConsoleFilter filter;
    filter.warn = false;
    CHECK_FALSE(ui::matches(warn, filter));

    filter = {};
    filter.text = "TEXTUR";
    CHECK(ui::matches(warn, filter));
    filter.text = "Holz";
    CHECK_FALSE(ui::matches(warn, filter));

    const log::RingSink::Entry fatal{log::Level::Fatal, {}, "Absturz"};
    filter = {};
    filter.error = false;
    CHECK_FALSE(ui::matches(fatal, filter));
}

TEST_CASE("Log console follows its sink and filter", "[editor_ui]") {
    const auto sink = std::make_shared<log::RingSink>(16);
    ui::LogConsole console{sink};
    const auto write = [&](log::Level level, const char* message) {
        sink->write({level, message, std::chrono::milliseconds{0}});
    };
    write(log::Level::Info, "eins");
    write(log::Level::Error, "zwei");
    console.refresh();
    CHECK(console.visible().size() == 2);

    console.filter.info = false;
    console.refresh();
    REQUIRE(console.visible().size() == 1);
    CHECK(console.visible()[0].message == "zwei");

    write(log::Level::Error, "drei");
    console.refresh();
    CHECK(console.visible().size() == 2);
    sink->clear();
    console.refresh();
    CHECK(console.visible().empty());
}

TEST_CASE("Viewport size follows its window one frame later", "[editor_ui]") {
    ui::ViewportSize size{{640, 360}};
    ui::Extent changed;
    CHECK_FALSE(size.take_change(changed));

    size.request(800.6f, 450.2f);
    CHECK(size.current() == ui::Extent{640, 360});
    REQUIRE(size.take_change(changed));
    CHECK(changed == ui::Extent{800, 450});
    CHECK(size.current() == ui::Extent{800, 450});
    CHECK_FALSE(size.take_change(changed));

    // Eingeklappt: alte Größe bleiben lassen; winzig: auf die kleinste Größe klemmen.
    size.request(0.0f, 300.0f);
    CHECK_FALSE(size.take_change(changed));
    size.request(3.0f, 2.0f);
    REQUIRE(size.take_change(changed));
    CHECK(changed == ui::Extent{ui::ViewportSize::min_side, ui::ViewportSize::min_side});
}
