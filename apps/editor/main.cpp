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
// Seit R2.3: Gizmos zum Verschieben, Drehen und Skalieren (EDIT-05), Text in der Welt (REND-15)
// und Spielen im Editor (EDIT-06). Menü, Dialoge und Kurzbefehle stehen in menu.cppm, die
// Werkzeuge des Viewports in viewport_tools.cppm.
//
// Seit R2.4: Asset-Browser (EDIT-09) mit Ziehen von Modellen in den Viewport, Assets über stabile
// Kennungen (ASSET-05), Laden im Hintergrund (ASSET-04) und Hot Reload von Modellen, Texturen und
// Shadern (PLAT-06). Asset- und Shader-Ordner sucht der Editor ab Arbeitsordner und Programmordner
// aufwärts („assets“ bzw. „engine/shaders“), wenn sie nicht angegeben sind.
//
// Seit R2.5: Modelle kommen gekocht aus dem Cache im Asset-Ordner; fehlt er oder ist er veraltet,
// kocht der Editor beim Laden (ASSET-02, ASSET-03). Menü „Prefab“: Auswahl als Prefab speichern,
// Instanz aufbrechen, Änderungen in die Vorlage übernehmen; Prefabs lassen sich aus dem
// Asset-Browser ziehen (WORLD-04). Szenen mit der Endung .nscene speichert er binär.
//
// Seit R3.4: Voxel-Werkzeuge (EDIT-08): Ist ein Terrain ausgewählt, zeigt die Werkzeugleiste den
// Pinsel zum Abtragen, Auftragen, Glätten, Ebnen und Bemalen mit Vorschau unter der Maus; ein
// Pinselstrich ist ein Schritt im Verlauf. Änderungen am Terrain speichert der Editor als Deltas in
// Regionen-Dateien neben der Szene. „Alles kochen“ läuft im Hintergrund (TD-34).
//
// Optionen:
//   --scene <pfad>  Szene öffnen statt der Standardszene
//   --save <pfad>   Szene beim Beenden dorthin speichern (für automatische Tests)
//   --model <pfad>  glTF-Modell statt des Kugelrasters
//   --frames <n>    nach n Frames beenden (für automatische Tests)
//   --no-vsync      ohne vertikale Synchronisation präsentieren
//   --baseline      optionale GPU-Stufen abschalten
//   --no-ini        Lage der Fenster weder laden noch speichern (für automatische Tests)
//   --play <n>      gleich spielen und nach n Frames stoppen (für automatische Tests: die Welt
//                   muss danach unverändert gespeichert werden)
//   --assets <pfad> Asset-Ordner
//   --shaders <pfad> Quellordner der Shader für Hot Reload
//   --insert <pfad> Modell oder Prefab aus dem Asset-Ordner beim Start einfügen wie per Ziehen
//                   (für automatische Tests)
//   --terrain       beim Start ein neues Terrain einfügen wie über „Bearbeiten → Neues Terrain“
//   --no-cache      Modelle nicht kochen und nicht aus dem Cache laden, sondern aus den Quellen

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
import ngine.world;
import ngine.editor_ui;
import app.model_scene;
import app.input_bindings;
import app.paths;
import app.prefabs;
import ngine.input;
import ngine.cook;
import editor.prefab_tools;
import ngine.asset;
import editor.assets;
import editor.input;
import editor.inspector;
import editor.menu;
import editor.overlay;
import editor.scene_tree;
import editor.terrain_tools;
import editor.viewport_tools;

namespace {

using namespace ngine;
using namespace editor;
namespace ui = editor_ui;

// Das Viewport-Bild ist sRGB, damit die UI es ohne Umrechnung zeigt.
constexpr rhi::Format viewport_format = rhi::Format::RGBA8Srgb;

struct Options {
    std::optional<std::string> scene_path;
    std::optional<std::string> save_path;
    std::optional<std::string> model_path;
    std::optional<std::string> assets_path;
    std::optional<std::string> shaders_path;
    std::optional<std::string> insert_path;
    bool terrain = false;
    std::optional<u64> frames;
    std::optional<u64> play_frames;
    bool vsync = true;
    bool baseline = false;
    bool ini = true;
    bool cache = true;
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
        } else if (arg == "--assets" && i + 1 < args.size()) {
            options.assets_path = std::string{args[++i]};
        } else if (arg == "--shaders" && i + 1 < args.size()) {
            options.shaders_path = std::string{args[++i]};
        } else if (arg == "--terrain") {
            options.terrain = true;
        } else if (arg == "--insert" && i + 1 < args.size()) {
            options.insert_path = std::string{args[++i]};
        } else if ((arg == "--frames" || arg == "--play") && i + 1 < args.size()) {
            const std::string_view value{args[++i]};
            u64 frames = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), frames).ec ==
                std::errc{}) {
                (arg == "--frames" ? options.frames : options.play_frames) = frames;
            }
        } else if (arg == "--no-vsync") {
            options.vsync = false;
        } else if (arg == "--baseline") {
            options.baseline = true;
        } else if (arg == "--no-ini") {
            options.ini = false;
        } else if (arg == "--no-cache") {
            options.cache = false;
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
        // Asset-Ordner und Shader-Quellen (R2.4). Der Ressourcen-Manager der Szene findet Assets
        // über die Datenbank; Szenen verweisen nur über Kennungen auf sie.
        std::optional<asset::AssetDatabase> database;
        if (const auto root =
                options.assets_path ? options.assets_path : app::find_upwards("assets")) {
            if (auto opened = asset::AssetDatabase::open(*root)) {
                database = std::move(*opened);
                log::info("Asset-Ordner: {} ({} Assets)", database->root(),
                          database->assets().size());
            } else {
                log::error("{}", opened.error());
            }
        } else {
            log::warn("Kein Asset-Ordner gefunden (Option --assets <ordner>)");
        }
        // Cache gekochter Modelle (R2.5): Der Editor kocht beim Laden, was fehlt oder veraltet ist.
        std::optional<asset::AssetCache> cache;
        if (database && options.cache) {
            cache.emplace(asset::default_cache_dir(database->root()));
            log::info("Cache gekochter Assets: {}", cache->directory());
        }
        app::SceneAssets scene_assets{.database = database ? &*database : nullptr, .loading = {}};
        if (cache) {
            scene_assets.loading = {.cache = &*cache, .cook = cook::model_cooker()};
        }
        auto scene =
            app::ModelScene::create(**device, viewport_format, options.model_path, math::DVec3{},
                                    app::SceneMode::Editor, std::move(scene_assets));
        if (!scene) {
            log::fatal("{}", scene.error());
            return 1;
        }
        const auto shader_dir = options.shaders_path
                                    ? options.shaders_path
                                    : app::find_upwards("engine/shaders", "pbr.slang");
        HotReload hot_reload{database ? &*database : nullptr, shader_dir};
        AssetPanel asset_panel;
        asset_panel.jobs = &(*scene)->jobs();
        ModelDrop model_drop;

        // Dokument, Auswahl und Ereignisse: Die Fenster kennen sich nicht, sie reden über den Bus.
        events::EventBus bus;
        edit::SceneDocument document{(*scene)->world(), &bus};
        document.set_prefabs((*scene)->prefabs().lookup());
        edit::Selection selection{&bus};
        editor::SceneTreePanel tree_panel;
        editor::InspectorPanel inspector_panel;
        const events::Subscription on_reset = bus.subscribe<edit::DocumentReset>(
            [&](const edit::DocumentReset&) { selection.clear(); });
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
        // Eingabe als Aktionen (R4.1): Tastatur und Maus nur beim Fliegen, das Gamepad immer.
        app::InputBindings bindings{database ? std::optional{database->root()} : std::nullopt};
        input::ActionState actions{bindings.load()};
        Panels panels;
        FrameTimes times;
        bool running = true;
        // Kamera fliegen: rechte Maustaste im Viewport gehalten.
        bool looking = false;
        bool viewport_hovered = false;
        u64 frame_count = 0;
        std::string title;
        ViewportTools tools;
        TerrainTools terrain_tools;
        // Dauer des laufenden Frames für Werkzeuge, die pro Sekunde wirken (Pinsel).
        f32 frame_dt = 0.0f;
        PlayControls play;
        EditorMenu menu{options.scene_path.value_or("szene.json")};
        MenuTargets targets{.document = document,
                            .selection = selection,
                            .tree = tree_panel,
                            .panels = panels,
                            .gizmo = tools.gizmo,
                            .play = play,
                            .quit = [&] { running = false; },
                            .extra_menus = {},
                            .new_terrain = [&] { editor::insert_terrain(document, selection); },
                            .terrain_brush = &terrain_tools.active};
        targets.extra_menus = [&] {
            const edit::EntityId selected = selection.current();
            const u64 entity = document.entity_of(selected);
            const bool in_instance =
                entity != 0 && world::instance_root(document.world(), entity) != 0;
            const bool editable = !document.read_only();
            if (ui::begin_menu("Prefab")) {
                if (ui::menu_item("Als Prefab speichern", {}, nullptr,
                                  editable && database && entity != 0 && !in_instance)) {
                    PrefabTools::save_as_prefab(document, selected, *database);
                }
                if (ui::menu_item("Änderungen in Vorlage übernehmen", {}, nullptr,
                                  database && in_instance)) {
                    PrefabTools::apply_to_prefab(document, selected, *database);
                }
                if (ui::menu_item("Prefab aufbrechen", {}, nullptr, editable && in_instance)) {
                    PrefabTools::unpack(document, selected);
                }
                ui::end_menu();
            }
            if (ui::begin_menu("Assets")) {
                const bool can_cook = database && cache && !asset_panel.cooking();
                if (ui::menu_item("Alles kochen", {}, nullptr, can_cook)) {
                    asset_panel.start_cook(*database, *cache, false);
                }
                if (ui::menu_item("Alles neu kochen", {}, nullptr, can_cook)) {
                    asset_panel.start_cook(*database, *cache, true);
                }
                ui::end_menu();
            }
        };
        auto last_time = std::chrono::steady_clock::now();
        // Neu berechnete Weltlagen im letzten Frame (TD-15), für die Statistik.
        usize transforms_updated = 0;

        // Fenster des Editors; dünn, die Logik liegt in Engine und Szene.
        const auto draw_ui = [&] {
            targets.terrain_selected = TerrainTools::selected_terrain(document, selection) != 0;
            menu.draw_menu_bar(targets);
            ui::dockspace(default_layout());

            viewport_hovered = false;
            if (panels.viewport) {
                const bool visible =
                    ui::begin_window(viewport_window, &panels.viewport,
                                     ui::WindowFlags::NoPadding | ui::WindowFlags::NoScrollbar |
                                         ui::WindowFlags::NoScrollWithMouse);
                if (visible) {
                    tools.draw_toolbar(document, play);
                    terrain_tools.draw_toolbar(document, selection);
                    const ui::Size available = ui::content_region_available();
                    viewport_size.request(available.width, available.height);
                    const ui::Extent extent = viewport_size.current();
                    ui::image_area(
                        "##Bild", ui::texture_id(*viewport.texture),
                        {static_cast<f32>(extent.width), static_cast<f32>(extent.height)});
                    viewport_hovered = ui::is_item_hovered();
                    const ui::Point corner = ui::item_min();
                    const auto& camera_view = (*scene)->renderer().camera_override;
                    // Modell aus dem Asset-Browser hineingezogen: am Boden unter der Maus
                    // einfügen, sobald es geladen ist.
                    if (const auto dropped = ui::drop_target(asset_payload)) {
                        const asset::AssetInfo* info =
                            database ? database->find(*dropped) : nullptr;
                        if (info != nullptr && camera_view && !document.read_only()) {
                            const ui::Point mouse = ui::mouse_position();
                            const math::DVec3 position =
                                drop_position(view_camera(*camera_view, extent), mouse.x - corner.x,
                                              mouse.y - corner.y);
                            if (info->type == asset::AssetType::Prefab) {
                                PrefabTools::insert(document, selection, (*scene)->prefabs(), *info,
                                                    position);
                            } else {
                                model_drop.start((*scene)->resources(), *info, position);
                            }
                        }
                    }
                    bool gizmo_used = false;
                    if (camera_view) {
                        const edit::ViewCamera camera = view_camera(*camera_view, extent);
                        // Der Pinsel geht vor: Ist er an, formt der Klick das Terrain.
                        gizmo_used =
                            !looking && terrain_tools.update(document, selection, camera, corner,
                                                             viewport_hovered, frame_dt);
                        if (terrain_tools.active) {
                            tools.gizmo.reset();
                        } else {
                            gizmo_used = !looking && tools.update(document, selection, camera,
                                                                  corner, viewport_hovered);
                        }
                        tools.draw(document, selection, camera, corner,
                                   (*scene)->renderer().debug().texts());
                        terrain_tools.draw(camera, corner);
                    }
                    // Klick ins Bild: Entity unter dem Mauszeiger aus dem ID-Puffer auswählen,
                    // wenn der Gizmo den Klick nicht für sich genommen hat.
                    if (!gizmo_used && !terrain_tools.active && viewport_hovered &&
                        ui::is_mouse_clicked(ui::MouseButton::Left)) {
                        const ui::Point mouse = ui::mouse_position();
                        const f32 x = mouse.x - corner.x;
                        const f32 y = mouse.y - corner.y;
                        if (x >= 0.0f && y >= 0.0f) {
                            (*scene)->renderer().request_pick(static_cast<u32>(x),
                                                              static_cast<u32>(y));
                        }
                    }
                    if (ui::is_window_focused() && !ui::is_any_item_active()) {
                        tools.handle_keys();
                        terrain_tools.handle_keys(document, selection);
                        if (ui::is_key_pressed(ui::Key::Delete)) {
                            EditorMenu::delete_selected(targets);
                        }
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
            if (panels.assets) {
                if (ui::begin_window(assets_window, &panels.assets)) {
                    asset_panel.draw(database ? &*database : nullptr, (*scene)->resources(),
                                     cache ? &*cache : nullptr);
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
                    ui::text(std::format("Instanzen: {} ({} Meshlets, {} Dreiecke)", stats.draws,
                                         stats.meshlets, stats.triangles));
                    ui::text(std::format("Gezeichnet: {} Meshlets, {} Dreiecke ({})",
                                         stats.meshlets_drawn, stats.triangles_drawn,
                                         stats.mesh_shaders ? "Mesh Shader" : "indirekt"));
                    ui::text(std::format("Aussortiert: {} Stumpf, {} Rückseiten, {} verdeckt",
                                         stats.culled_frustum, stats.culled_backface,
                                         stats.culled_occluded));
                    ui::text(std::format("Schatten-Draws: {}", stats.shadow_draws));
                    ui::text(std::format("Lichter: {}", stats.lights));
                    ui::text(std::format("Wartet auf Modelle: {}", stats.waiting));
                    ui::text(std::format("UI-Draws: {}", (*ui_renderer)->draw_count()));
                    ui::separator();
                    ui::text(std::format("Entities: {}", document.world().entity_count()));
                    ui::text(std::format("Verlauf: {} von {}", document.history().position(),
                                         document.history().size()));
                    ui::text(std::format("Neu berechnete Weltlagen: {}", transforms_updated));
                    // Deltas des ausgewählten Terrains (R3.4).
                    if (const edit::EntityId terrain =
                            TerrainTools::selected_terrain(document, selection);
                        terrain != 0) {
                        const world::Entity entity =
                            document.world().entity(document.entity_of(terrain));
                        if (const auto* state = entity.get<world::TerrainState>();
                            state != nullptr && state->data) {
                            const world::TerrainStats& terrain_stats = state->data->stats;
                            ui::text(std::format("Terrain: {} Chunks, {} Dreiecke",
                                                 terrain_stats.chunks, terrain_stats.triangles));
                            ui::text(std::format(
                                "Deltas: {} Chunks, {:.1f} KiB", terrain_stats.delta_chunks,
                                static_cast<f64>(terrain_stats.delta_bytes) / 1024.0));
                        }
                    }
                    ui::separator();
                    ui::text_disabled("Kamera: rechte Maustaste im Viewport halten,");
                    ui::text_disabled("W/A/S/D, E/Q, Shift schneller");
                    ui::text_disabled("Linksklick ins Bild wählt aus");
                    ui::text_disabled("W/E/R: Verschieben, Drehen, Skalieren");
                    ui::text_disabled("Strg beim Ziehen: Raster, F5: Spielen");
                    ui::text_disabled(
                        "Terrain-Pinsel: Terrain auswählen, dann „Pinsel“ über dem Viewport "
                        "oder Werkzeug > Terrain-Pinsel");
                }
                ui::end_window();
            }

            menu.draw_dialogs(targets);
            menu.handle_shortcuts(targets);
            // Ein Regler oder Gizmo, der losgelassen ist, beendet seinen Schritt im Verlauf.
            if (!ui::is_any_item_active() && !tools.gizmo.dragging() &&
                !terrain_tools.tool.stroking()) {
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
            frame_dt = dt;

            // Neue Größe aus dem letzten Frame übernehmen, bevor die UI auf die Textur zeigt.
            if (ui::Extent extent; viewport_size.take_change(extent)) {
                if (auto result = viewport.resize(**device, extent); !result) {
                    log::error("Viewport: {}", result.error());
                }
            }

            // Geänderte Dateien neu laden, ein gezogenes Modell einfügen, sobald es da ist.
            hot_reload.poll(database ? &*database : nullptr, (*scene)->resources(),
                            (*scene)->renderer());
            model_drop.update(document, selection);
            asset_panel.poll();
            // Geänderte Vorlagen: Instanzen nachziehen (Überschreibungen bleiben).
            if ((*scene)->update_prefabs() > 0) {
                document.notify(edit::EntityChanged{});
            }

            // Spielen: Systeme der Szene laufen lassen. Danach Weltlagen und Hilfslinien samt
            // Texten in der Welt, damit die UI die Texte dieses Frames zeichnet.
            if (document.playing()) {
                world::Simulation& simulation = (*scene)->simulation();
                simulation.set_paused(play.paused);
                if (std::exchange(play.step_requested, false)) {
                    simulation.step();
                }
                (*scene)->simulate(dt);
            } else {
                (*scene)->simulation().reset_clock();
            }
            transforms_updated = world::update_transforms(document.world());
            render::DebugDraw& debug = (*scene)->renderer().debug();
            debug.clear();
            editor::draw_overlay(debug, (*scene)->renderer(), document, selection);

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
            const std::string new_title =
                std::format("NGine Editor – {}{}{}", file_label(document),
                            document.dirty() ? " *" : "", document.playing() ? " [Spielt]" : "");
            if (new_title != title) {
                title = new_title;
                window->set_title(title);
            }
            window->set_text_input(context.wants_text_input());

            actions.end_frame(dt);
            (*scene)->update(dt, app::fly_input(actions));

            if (auto frame = (*device)->begin_frame()) {
                gpu_profiler.begin_frame(*frame);
                render::RenderGraph graph;
                const auto target = graph.import_texture(
                    "Viewport", *viewport.texture,
                    viewport.fresh ? rhi::ResourceState::Undefined : rhi::ResourceState::ShaderRead,
                    rhi::ResourceState::ShaderRead);
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

        const auto stop_looking = [&] {
            looking = false;
            actions.release_all();
            window->set_relative_mouse(false);
        };

        if (options.play_frames) {
            play.start(document, tools.gizmo);
        }
        if (options.terrain) {
            editor::insert_terrain(document, selection);
        }
        if (options.insert_path) {
            const asset::AssetInfo* info =
                database ? database->find_path(*options.insert_path) : nullptr;
            if (info != nullptr && info->type == asset::AssetType::Prefab) {
                PrefabTools::insert(document, selection, (*scene)->prefabs(), *info, math::DVec3{});
            } else if (info != nullptr) {
                model_drop.start((*scene)->resources(), *info, math::DVec3{});
            } else {
                log::error("--insert: {} gibt es im Asset-Ordner nicht", *options.insert_path);
            }
        }
        while (running) {
            namespace event = platform::event;
            bindings.poll(actions);
            actions.begin_frame();
            for (const platform::Event& e : window->poll_events()) {
                // Beim Fliegen gehören Tastatur und Maus der Kamera; Loslassen, Fokus und Gamepad
                // zählen immer.
                const bool always = std::holds_alternative<event::KeyReleased>(e) ||
                                    std::holds_alternative<event::MouseButtonReleased>(e) ||
                                    std::holds_alternative<event::FocusChanged>(e) ||
                                    std::holds_alternative<event::GamepadAdded>(e) ||
                                    std::holds_alternative<event::GamepadRemoved>(e) ||
                                    std::holds_alternative<event::GamepadButtonPressed>(e) ||
                                    std::holds_alternative<event::GamepadButtonReleased>(e) ||
                                    std::holds_alternative<event::GamepadAxisMoved>(e);
                if (looking || always) {
                    actions.handle(e);
                }
                std::visit(
                    [&](const auto& ev) {
                        using T = std::decay_t<decltype(ev)>;
                        if constexpr (std::is_same_v<T, event::Quit>) {
                            menu.request_quit(targets);
                        } else if constexpr (std::is_same_v<T, event::Resized>) {
                            (*device)->resize({ev.framebuffer.width, ev.framebuffer.height});
                        } else if constexpr (std::is_same_v<T, event::KeyPressed> ||
                                             std::is_same_v<T, event::KeyReleased>) {
                            constexpr bool down = std::is_same_v<T, event::KeyPressed>;
                            if (looking && down && ev.key == platform::Key::Escape) {
                                stop_looking();
                            }
                            if (!looking || !down) {
                                if (const auto key = editor::to_ui(ev.key)) {
                                    context.add_key(*key, down);
                                }
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseMoved>) {
                            if (!looking) {
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
            if (options.play_frames && document.playing() && frame_count >= *options.play_frames) {
                play.stop(document);
            }
            // Mit --frames erst beenden, wenn ein eingefügtes Modell auch da ist.
            if (options.frames && frame_count >= *options.frames && !model_drop.pending()) {
                running = false;
            }
        }

        window->set_redraw_callback({});
        (*device)->wait_idle();
        if (document.playing()) {
            play.stop(document);
        }
        if (options.save_path) {
            menu.save_to(document, *options.save_path);
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
