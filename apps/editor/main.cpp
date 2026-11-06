// Editor (R2): eigene Anwendung mit Dear ImGui und Docking (EDIT-01). Das Viewport-Fenster zeigt
// die Szene, die der Renderer in eine Textur zeichnet (EDIT-02); die Größe folgt dem Fenster.
// Freie Kamera wie in der Sandbox: rechte Maustaste im Viewport halten und Maus bewegen, W/A/S/D
// fliegen, E/Q hoch und runter, Shift schneller. Die Kamera des Editors gehört nicht zur Szene.
// Die Konsole zeigt das Log (EDIT-10), das zusätzlich in eine Datei im Benutzerordner geht
// (CORE-12).
//
// Seit R2.2: Szenenbaum (EDIT-03), Inspector aus der Reflection (EDIT-04), Auswahl per Klick im
// Viewport über den ID-Puffer, Undo/Redo als Befehle (EDIT-07), Szene öffnen und speichern. Die
// Teile reden über Ereignisse (CORE-11) miteinander.
//
// Optionen:
//   --scene <pfad>  Szene öffnen statt der Standardszene
//   --save <pfad>   Szene beim Beenden dorthin speichern (für automatische Tests)
//   --model <pfad>  glTF-Modell statt des Kugelrasters
//   --frames <n>    nach n Frames beenden (für automatische Tests)
//   --no-vsync      ohne vertikale Synchronisation präsentieren
//   --baseline      optionale GPU-Stufen abschalten
//   --no-ini        Lage der Fenster weder laden noch speichern (für automatische Tests)

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

import ngine.core;
import ngine.edit;
import ngine.filesystem;
import ngine.math;
import ngine.platform;
import ngine.profile;
import ngine.render;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.editor_ui;
import app.model_scene;
import editor.input;
import editor.inspector;
import editor.overlay;
import editor.scene_tree;

namespace {

using namespace ngine;
namespace ui = editor_ui;

// Das Viewport-Bild ist sRGB, damit die UI es ohne Umrechnung zeigt.
constexpr rhi::Format viewport_format = rhi::Format::RGBA8Srgb;

constexpr std::string_view viewport_window = "Viewport";
constexpr std::string_view console_window = "Konsole";
constexpr std::string_view stats_window = "Statistik";
constexpr std::string_view tree_window = "Szene";
constexpr std::string_view inspector_window = "Inspector";

struct Options {
    std::optional<std::string> scene_path;
    std::optional<std::string> save_path;
    std::optional<std::string> model_path;
    std::optional<u64> frames;
    bool vsync = true;
    bool baseline = false;
    bool ini = true;
};

Options parse_options(std::span<char*> args) {
    Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "--scene" && i + 1 < args.size()) {
            options.scene_path = std::string{args[++i]};
        } else if (arg == "--save" && i + 1 < args.size()) {
            options.save_path = std::string{args[++i]};
        } else if (arg == "--model" && i + 1 < args.size()) {
            options.model_path = std::string{args[++i]};
        } else if (arg == "--frames" && i + 1 < args.size()) {
            const std::string_view value{args[++i]};
            u64 frames = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), frames).ec ==
                std::errc{}) {
                options.frames = frames;
            }
        } else if (arg == "--no-vsync") {
            options.vsync = false;
        } else if (arg == "--baseline") {
            options.baseline = true;
        } else if (arg == "--no-ini") {
            options.ini = false;
        } else {
            log::warn("Unbekannte Option: {}", arg);
        }
    }
    return options;
}

// Textur, in die der Renderer die Szene zeichnet und die das Viewport-Fenster zeigt.
struct ViewportTarget {
    std::unique_ptr<rhi::Texture> texture;
    // Eine neue Textur hat noch keinen Inhalt (Zustand Undefined), danach bleibt sie zwischen den
    // Frames im Zustand ShaderRead.
    bool fresh = true;

    rhi::Result<void> resize(rhi::Device& device, ui::Extent extent) {
        auto created = device.create_texture(
            {.extent = {extent.width, extent.height},
             .format = viewport_format,
             .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled});
        if (!created) {
            return std::unexpected(created.error());
        }
        // Die alte Textur gibt das Device frei, wenn die GPU mit ihr fertig ist.
        texture = *std::move(created);
        fresh = true;
        return {};
    }
};

// Zeiten der letzten Frames für die Statistik.
struct FrameTimes {
    f32 average_ms = 0.0f;
    f32 accumulated = 0.0f;
    u32 count = 0;

    void add(f32 dt) {
        accumulated += dt;
        ++count;
        if (accumulated >= 0.5f) {
            average_ms = accumulated * 1000.0f / static_cast<f32>(count);
            accumulated = 0.0f;
            count = 0;
        }
    }
};

// Sichtbarkeit der Fenster (Menü „Ansicht“).
struct Panels {
    bool viewport = true;
    bool tree = true;
    bool inspector = true;
    bool console = true;
    bool stats = true;
};

ui::DockLayout default_layout() {
    return {.center = {std::string{viewport_window}},
            .left = {std::string{tree_window}},
            // Das zuletzt angedockte Fenster liegt vorn.
            .right = {std::string{stats_window}, std::string{inspector_window}},
            .bottom = {std::string{console_window}}};
}

// Aktion, die bei ungespeicherten Änderungen erst nachfragt.
enum class Pending : u8 { None, New, Open, Quit };

// Dateiname für Titel und Meldungen.
std::string file_label(const edit::SceneDocument& document) {
    if (document.path().empty()) {
        return "Unbenannt";
    }
    const std::string& path = document.path();
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Log-Datei im Benutzerordner (CORE-12); ohne Benutzerordner im Arbeitsverzeichnis.
void add_file_log() {
    auto dir = filesystem::user_data_dir("NGine");
    const std::string path =
        dir ? filesystem::join(*dir, "editor.log") : std::string{"ngine-editor.log"};
    auto sink = log::FileSink::create(path);
    if (!sink) {
        log::warn("{}", sink.error());
        return;
    }
    log::add_sink(*std::move(sink));
    log::info("Log-Datei: {}", path);
}

std::string ini_path(const Options& options) {
    if (!options.ini) {
        return {};
    }
    auto dir = filesystem::user_data_dir("NGine");
    return dir ? filesystem::join(*dir, "editor.ini") : std::string{"ngine-editor.ini"};
}

int run(const Options& options, const std::shared_ptr<log::RingSink>& log_ring) {
    auto window = platform::Window::create({.title = "NGine Editor", .width = 1600, .height = 900});
    if (!window) {
        log::fatal("{}", window.error());
        return 1;
    }
    const auto size = window->framebuffer_size();
    auto device = rhi::vulkan::create_device({
        .application_name = "NGine Editor",
#ifdef NDEBUG
        .enable_validation = false,
#else
        .enable_validation = true,
#endif
        .vsync = options.vsync,
        .surface =
            rhi::SurfaceDesc{
                .instance_extensions = window->vulkan_instance_extensions(),
                .create_surface =
                    [&](void* instance) { return window->create_vulkan_surface(instance); },
                .extent = {size.width, size.height},
            },
        .disabled_features = {.mesh_shader = options.baseline, .ray_tracing = options.baseline},
    });
    if (!device) {
        log::fatal("{}", device.error());
        return 1;
    }

    int exit_code = 0;
    {
        // Alles, was GPU-Ressourcen hält, lebt in diesem Block und endet vor dem Device. Der
        // UI-Kontext gibt seine Texturen an den UiRenderer zurück und endet deshalb vor ihm.
        auto ui_renderer = ui::UiRenderer::create(**device, (*device)->swapchain_format());
        if (!ui_renderer) {
            log::fatal("{}", ui_renderer.error());
            return 1;
        }
        auto scene = app::ModelScene::create(**device, viewport_format, options.model_path,
                                             math::DVec3{}, app::SceneMode::Editor);
        if (!scene) {
            log::fatal("{}", scene.error());
            return 1;
        }

        // Dokument, Auswahl und Ereignisse: Die Fenster kennen sich nicht, sie reden über den Bus.
        events::EventBus bus;
        edit::SceneDocument document{(*scene)->world(), &bus};
        edit::Selection selection{&bus};
        editor::SceneTreePanel tree_panel;
        editor::InspectorPanel inspector_panel;
        const events::Subscription on_reset =
            bus.subscribe<edit::DocumentReset>([&](const edit::DocumentReset&) {
                selection.clear();
                (*scene)->forget_animated_lights();
            });
        const events::Subscription on_change = bus.subscribe<edit::EntityChanged>(
            [&](const edit::EntityChanged&) { selection.validate(document); });
        const events::Subscription on_select =
            bus.subscribe<edit::SelectionChanged>([&](const edit::SelectionChanged& change) {
                if (change.current != 0) {
                    log::debug("Ausgewählt: {}", change.current);
                }
            });
        if (options.scene_path) {
            if (auto opened = document.open(*options.scene_path); opened) {
                log::info("Szene geöffnet: {} ({} Entities)", *options.scene_path, *opened);
            } else {
                log::error("{}", opened.error());
            }
        }
        const std::string layout_file = ini_path(options);
        ui::Context context{
            **ui_renderer,
            {.ini_path = layout_file,
             .scale = window->display_scale(),
             .get_clipboard = [&] { return window->clipboard_text(); },
             .set_clipboard = [&](const std::string& text) { window->set_clipboard_text(text); }}};
        ui::LogConsole console{log_ring};

        ui::ViewportSize viewport_size;
        ViewportTarget viewport;
        if (auto result = viewport.resize(**device, viewport_size.current()); !result) {
            log::fatal("Viewport: {}", result.error());
            return 1;
        }

        render::TexturePool pool{**device};
        render::GpuProfiler gpu_profiler{**device};
        app::FlyInput fly;
        Panels panels;
        FrameTimes times;
        bool running = true;
        // Kamera fliegen: rechte Maustaste im Viewport gehalten.
        bool looking = false;
        bool viewport_hovered = false;
        u64 frame_count = 0;
        // Pfad im Dialog „Öffnen“ bzw. „Speichern unter“.
        std::string path_input = options.scene_path.value_or("szene.json");
        enum class PathDialog : u8 { None, Open, SaveAs };
        PathDialog path_dialog = PathDialog::None;
        bool open_path_dialog = false;
        Pending pending = Pending::None;
        bool open_confirm = false;
        std::string title;

        const auto save_to = [&](const std::string& path) {
            if (auto result = document.save(path); result) {
                log::info("Szene gespeichert: {}", path);
            } else {
                log::error("{}", result.error());
            }
        };
        const auto save = [&] {
            if (document.path().empty()) {
                path_dialog = PathDialog::SaveAs;
                open_path_dialog = true;
            } else {
                save_to(document.path());
            }
        };
        const auto do_action = [&](Pending action) {
            switch (action) {
            case Pending::New: document.clear(); break;
            case Pending::Open:
                path_dialog = PathDialog::Open;
                open_path_dialog = true;
                break;
            case Pending::Quit: running = false; break;
            case Pending::None: break;
            }
        };
        // Verwirft ungespeicherte Änderungen erst nach Rückfrage.
        const auto request = [&](Pending action) {
            if (document.dirty()) {
                pending = action;
                open_confirm = true;
            } else {
                do_action(action);
            }
        };
        const auto undo = [&] {
            if (!document.undo()) {
                log::debug("Nichts rückgängig zu machen");
            }
        };
        const auto redo = [&] {
            if (!document.redo()) {
                log::debug("Nichts zu wiederholen");
            }
        };
        const auto delete_selected = [&] {
            if (selection.current() == 0) {
                return;
            }
            if (auto result = document.execute(edit::destroy_entity(selection.current()));
                !result) {
                log::error("{}", result.error());
            }
        };

        // Kurzbefehle; nicht, solange ein Eingabefeld aktiv ist (dort gehören Strg+Z usw. dem
        // Feld).
        const auto handle_shortcuts = [&] {
            if (ui::is_any_item_active() || !ui::is_key_down(ui::Key::Ctrl)) {
                return;
            }
            const bool shift = ui::is_key_down(ui::Key::Shift);
            if (ui::is_key_pressed(ui::Key::Z, true)) {
                shift ? redo() : undo();
            } else if (ui::is_key_pressed(ui::Key::Y, true)) {
                redo();
            } else if (ui::is_key_pressed(ui::Key::S)) {
                save();
            } else if (ui::is_key_pressed(ui::Key::O)) {
                request(Pending::Open);
            } else if (ui::is_key_pressed(ui::Key::N)) {
                request(Pending::New);
            }
        };

        const auto draw_dialogs = [&] {
            if (open_path_dialog) {
                ui::open_popup(path_dialog == PathDialog::Open ? "Szene öffnen"
                                                               : "Szene speichern unter");
                open_path_dialog = false;
                if (path_dialog == PathDialog::SaveAs && !document.path().empty()) {
                    path_input = document.path();
                }
            }
            for (const PathDialog kind : {PathDialog::Open, PathDialog::SaveAs}) {
                const std::string_view name =
                    kind == PathDialog::Open ? "Szene öffnen" : "Szene speichern unter";
                if (!ui::begin_popup_modal(name)) {
                    continue;
                }
                ui::text("Pfad der Szene (JSON):");
                ui::set_next_item_width(420.0f);
                const bool entered = ui::input_text("##Pfad", path_input, true);
                const bool confirmed = ui::button("OK") || entered;
                ui::same_line();
                if (ui::button("Abbrechen") || ui::is_key_pressed(ui::Key::Escape)) {
                    path_dialog = PathDialog::None;
                    ui::close_current_popup();
                } else if (confirmed && !path_input.empty()) {
                    if (kind == PathDialog::Open) {
                        if (auto opened = document.open(path_input); opened) {
                            log::info("Szene geöffnet: {} ({} Entities)", path_input, *opened);
                        } else {
                            log::error("{}", opened.error());
                        }
                    } else {
                        save_to(path_input);
                    }
                    path_dialog = PathDialog::None;
                    ui::close_current_popup();
                }
                ui::end_popup();
            }

            if (open_confirm) {
                ui::open_popup("Ungespeicherte Änderungen");
                open_confirm = false;
            }
            if (ui::begin_popup_modal("Ungespeicherte Änderungen")) {
                ui::text(std::format("{} hat ungespeicherte Änderungen.", file_label(document)));
                if (ui::button("Verwerfen")) {
                    const Pending action = pending;
                    pending = Pending::None;
                    ui::close_current_popup();
                    do_action(action);
                }
                ui::same_line();
                if (ui::button("Abbrechen") || ui::is_key_pressed(ui::Key::Escape)) {
                    pending = Pending::None;
                    ui::close_current_popup();
                }
                ui::end_popup();
            }
        };
        auto last_time = std::chrono::steady_clock::now();

        // Fenster des Editors; dünn, die Logik liegt in Engine und Szene.
        const auto draw_ui = [&] {
            if (ui::begin_main_menu_bar()) {
                if (ui::begin_menu("Datei")) {
                    if (ui::menu_item("Neu", "Strg+N")) {
                        request(Pending::New);
                    }
                    if (ui::menu_item("Öffnen …", "Strg+O")) {
                        request(Pending::Open);
                    }
                    if (ui::menu_item("Speichern", "Strg+S")) {
                        save();
                    }
                    if (ui::menu_item("Speichern unter …")) {
                        path_dialog = PathDialog::SaveAs;
                        open_path_dialog = true;
                    }
                    ui::separator();
                    if (ui::menu_item("Beenden", "Alt+F4")) {
                        request(Pending::Quit);
                    }
                    ui::end_menu();
                }
                if (ui::begin_menu("Bearbeiten")) {
                    const edit::CommandStack& history = document.history();
                    const std::string undo_label = history.can_undo()
                                                       ? "Rückgängig: " + history.undo_label()
                                                       : std::string{"Rückgängig"};
                    const std::string redo_label = history.can_redo()
                                                       ? "Wiederholen: " + history.redo_label()
                                                       : std::string{"Wiederholen"};
                    if (ui::menu_item(undo_label, "Strg+Z", nullptr, history.can_undo())) {
                        undo();
                    }
                    if (ui::menu_item(redo_label, "Strg+Y", nullptr, history.can_redo())) {
                        redo();
                    }
                    ui::separator();
                    if (ui::menu_item("Neue Entity")) {
                        tree_panel.create(document, selection, 0);
                    }
                    if (ui::menu_item("Umbenennen", "F2", nullptr, selection.current() != 0)) {
                        tree_panel.start_rename(document, selection.current());
                    }
                    if (ui::menu_item("Löschen", "Entf", nullptr, selection.current() != 0)) {
                        delete_selected();
                    }
                    ui::end_menu();
                }
                if (ui::begin_menu("Ansicht")) {
                    ui::menu_item(viewport_window, {}, &panels.viewport);
                    ui::menu_item(tree_window, {}, &panels.tree);
                    ui::menu_item(inspector_window, {}, &panels.inspector);
                    ui::menu_item(console_window, {}, &panels.console);
                    ui::menu_item(stats_window, {}, &panels.stats);
                    ui::separator();
                    if (ui::menu_item("Layout zurücksetzen")) {
                        panels = {};
                        ui::reset_layout();
                    }
                    ui::end_menu();
                }
                ui::end_main_menu_bar();
            }
            ui::dockspace(default_layout());

            viewport_hovered = false;
            if (panels.viewport) {
                const bool visible =
                    ui::begin_window(viewport_window, &panels.viewport,
                                     ui::WindowFlags::NoPadding | ui::WindowFlags::NoScrollbar |
                                         ui::WindowFlags::NoScrollWithMouse);
                if (visible) {
                    const ui::Size available = ui::content_region_available();
                    viewport_size.request(available.width, available.height);
                    const ui::Extent extent = viewport_size.current();
                    ui::image(ui::texture_id(*viewport.texture),
                              {static_cast<f32>(extent.width), static_cast<f32>(extent.height)});
                    viewport_hovered = ui::is_item_hovered();
                    // Klick ins Bild: Entity unter dem Mauszeiger aus dem ID-Puffer auswählen.
                    if (viewport_hovered && ui::is_mouse_clicked(ui::MouseButton::Left)) {
                        const ui::Point mouse = ui::mouse_position();
                        const ui::Point corner = ui::item_min();
                        const f32 x = mouse.x - corner.x;
                        const f32 y = mouse.y - corner.y;
                        if (x >= 0.0f && y >= 0.0f) {
                            (*scene)->renderer().request_pick(static_cast<u32>(x),
                                                              static_cast<u32>(y));
                        }
                    }
                    if (ui::is_window_focused() && !ui::is_any_item_active() &&
                        ui::is_key_pressed(ui::Key::Delete)) {
                        delete_selected();
                    }
                }
                ui::end_window();
            }
            if (panels.tree) {
                if (ui::begin_window(tree_window, &panels.tree)) {
                    tree_panel.draw(document, selection);
                }
                ui::end_window();
            }
            if (panels.inspector) {
                if (ui::begin_window(inspector_window, &panels.inspector)) {
                    inspector_panel.draw(document, selection);
                }
                ui::end_window();
            }
            if (panels.console) {
                if (ui::begin_window(console_window, &panels.console)) {
                    console.draw();
                }
                ui::end_window();
            }
            if (panels.stats) {
                if (ui::begin_window(stats_window, &panels.stats)) {
                    const render::RenderStats& stats = (*scene)->renderer().stats();
                    const ui::Extent extent = viewport_size.current();
                    ui::text(
                        std::format("Frame: {:.2f} ms ({:.0f} FPS)", times.average_ms,
                                    times.average_ms > 0.0f ? 1000.0f / times.average_ms : 0.0f));
                    ui::text(std::format("Viewport: {} x {}", extent.width, extent.height));
                    ui::separator();
                    ui::text(std::format("Draws: {}", stats.draws));
                    ui::text(std::format("Dreiecke: {}", stats.triangles));
                    ui::text(std::format("Schatten-Draws: {}", stats.shadow_draws));
                    ui::text(std::format("Lichter: {}", stats.lights));
                    ui::text(std::format("UI-Draws: {}", (*ui_renderer)->draw_count()));
                    ui::separator();
                    ui::text(std::format("Entities: {}", document.world().entity_count()));
                    ui::text(std::format("Verlauf: {} von {}", document.history().position(),
                                         document.history().size()));
                    ui::separator();
                    ui::text_disabled("Kamera: rechte Maustaste im Viewport halten,");
                    ui::text_disabled("W/A/S/D, E/Q, Shift schneller");
                    ui::text_disabled("Linksklick ins Bild wählt aus");
                }
                ui::end_window();
            }

            draw_dialogs();
            handle_shortcuts();
            // Ein Regler, der losgelassen ist, beendet seinen Schritt im Verlauf.
            if (!ui::is_any_item_active()) {
                document.history().seal();
            }
        };

        // Ein ganzer Frame: Größe des Viewports, UI, Szene, Zeichnen. Läuft aus der Hauptschleife
        // und unter Windows aus dem Rückruf beim Ziehen des Fensters (TD-19).
        const auto run_frame = [&] {
            const auto now = std::chrono::steady_clock::now();
            const f32 dt = std::chrono::duration<f32>(now - last_time).count();
            last_time = now;
            times.add(dt);

            // Neue Größe aus dem letzten Frame übernehmen, bevor die UI auf die Textur zeigt.
            if (ui::Extent extent; viewport_size.take_change(extent)) {
                if (auto result = viewport.resize(**device, extent); !result) {
                    log::error("Viewport: {}", result.error());
                }
            }

            const auto framebuffer = window->framebuffer_size();
            context.begin_frame(static_cast<f32>(framebuffer.width),
                                static_cast<f32>(framebuffer.height), dt);
            draw_ui();
            const ui::UiDrawData& draw_data = context.end_frame();

            if (!looking) {
                window->set_cursor(editor::to_platform(context.cursor()));
            }
            bus.dispatch();
            if (auto pick = (*scene)->renderer().take_pick((*scene)->world())) {
                selection.select(pick->entity != 0 ? document.id_of(pick->entity) : 0);
            }
            const std::string new_title = std::format("NGine Editor – {}{}", file_label(document),
                                                      document.dirty() ? " *" : "");
            if (new_title != title) {
                title = new_title;
                window->set_title(title);
            }
            window->set_text_input(context.wants_text_input());

            (*scene)->update(dt, looking ? fly : app::FlyInput{});
            fly.look_x = 0.0f;
            fly.look_y = 0.0f;

            if (auto frame = (*device)->begin_frame()) {
                gpu_profiler.begin_frame(*frame);
                render::RenderGraph graph;
                const auto target = graph.import_texture(
                    "Viewport", *viewport.texture,
                    viewport.fresh ? rhi::ResourceState::Undefined : rhi::ResourceState::ShaderRead,
                    rhi::ResourceState::ShaderRead);
                editor::draw_overlay((*scene)->renderer().debug(), (*scene)->renderer().meshes(),
                                     document, selection);
                (*scene)->add_passes(graph, target);
                const auto backbuffer = graph.import_texture("Backbuffer", frame->backbuffer,
                                                             rhi::ResourceState::Undefined,
                                                             rhi::ResourceState::Present);
                const render::TextureHandle reads[] = {target};
                (*ui_renderer)
                    ->add_pass(graph, draw_data, backbuffer, reads,
                               rhi::Color{0.06f, 0.06f, 0.07f});
                graph.execute(frame->commands, pool, &gpu_profiler);
                pool.end_frame();
                (*device)->end_frame();
                viewport.fresh = false;
                ++frame_count;
            } else {
                // Minimiert oder Swapchain wird neu aufgebaut: nicht im Leerlauf kreiseln.
                std::this_thread::sleep_for(std::chrono::milliseconds{16});
            }
            profile::frame_mark();
        };
        window->set_redraw_callback([&](platform::Extent extent) {
            (*device)->resize({extent.width, extent.height});
            run_frame();
        });

        const auto set_fly_key = [&](platform::Key key, bool down) {
            using platform::Key;
            switch (key) {
            case Key::W: fly.forward = down; break;
            case Key::S: fly.back = down; break;
            case Key::A: fly.left = down; break;
            case Key::D: fly.right = down; break;
            case Key::E: fly.up = down; break;
            case Key::Q: fly.down = down; break;
            case Key::Shift: fly.fast = down; break;
            default: break;
            }
        };
        const auto stop_looking = [&] {
            looking = false;
            fly = {};
            window->set_relative_mouse(false);
        };

        while (running) {
            namespace event = platform::event;
            for (const platform::Event& e : window->poll_events()) {
                std::visit(
                    [&](const auto& ev) {
                        using T = std::decay_t<decltype(ev)>;
                        if constexpr (std::is_same_v<T, event::Quit>) {
                            request(Pending::Quit);
                        } else if constexpr (std::is_same_v<T, event::Resized>) {
                            (*device)->resize({ev.framebuffer.width, ev.framebuffer.height});
                        } else if constexpr (std::is_same_v<T, event::KeyPressed> ||
                                             std::is_same_v<T, event::KeyReleased>) {
                            constexpr bool down = std::is_same_v<T, event::KeyPressed>;
                            // Beim Fliegen gehören die Tasten der Kamera, Loslassen immer.
                            if (looking || !down) {
                                set_fly_key(ev.key, down);
                            }
                            if (looking && down && ev.key == platform::Key::Escape) {
                                stop_looking();
                            }
                            if (!looking || !down) {
                                if (const auto key = editor::to_ui(ev.key)) {
                                    context.add_key(*key, down);
                                }
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseMoved>) {
                            if (looking) {
                                fly.look_x += ev.dx;
                                fly.look_y += ev.dy;
                            } else {
                                context.add_mouse_position(ev.x, ev.y);
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseButtonPressed>) {
                            if (ev.button == platform::MouseButton::Right && viewport_hovered &&
                                !looking) {
                                looking = true;
                                window->set_relative_mouse(true);
                            } else if (!looking) {
                                if (const auto button = editor::to_ui(ev.button)) {
                                    context.add_mouse_button(*button, true);
                                }
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseButtonReleased>) {
                            if (ev.button == platform::MouseButton::Right && looking) {
                                stop_looking();
                            } else if (const auto button = editor::to_ui(ev.button)) {
                                context.add_mouse_button(*button, false);
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseWheel>) {
                            if (!looking) {
                                context.add_mouse_wheel(ev.delta, ev.delta_x);
                            }
                        } else if constexpr (std::is_same_v<T, event::TextInput>) {
                            if (!looking) {
                                context.add_text(ev.text);
                            }
                        } else if constexpr (std::is_same_v<T, event::FocusChanged>) {
                            context.add_focus(ev.focused);
                            if (!ev.focused && looking) {
                                stop_looking();
                            }
                        }
                    },
                    e);
            }

            run_frame();
            if (options.frames && frame_count >= *options.frames) {
                running = false;
            }
        }

        window->set_redraw_callback({});
        (*device)->wait_idle();
        if (options.save_path) {
            save_to(*options.save_path);
        }
        context.save_layout();
        log::info("Szene: {}", (*scene)->stats());
        log::info("{} Frames gezeichnet", frame_count);
    }

    if (const auto errors = (*device)->validation_errors(); errors > 0) {
        log::error("Die Validation Layers haben {} Fehler gemeldet", errors);
        exit_code = 1;
    }
    return exit_code;
}

} // namespace

int main(int argc, char** argv) {
    // Die Konsole des Editors sieht alles ab dem Start.
    const auto log_ring = std::make_shared<ngine::log::RingSink>(4000);
    ngine::log::add_sink(log_ring);
    const Options options = parse_options(std::span{argv, static_cast<std::size_t>(argc)});
    add_file_log();

    const auto version = ngine::engine_version;
    ngine::log::info("{} Editor {}.{}.{} startet", ngine::engine_name, version.major, version.minor,
                     version.patch);
    const int exit_code = run(options, log_ring);
    ngine::log::info("Beendet");
    ngine::log::flush();
    return exit_code;
}
