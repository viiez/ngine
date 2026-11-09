// Sandbox: kleine Testanwendung, an der jede Iteration sichtbar wird.
//
// Ohne Demo-Option zeigt die Sandbox ein Raster aus Kugeln mit steigendem Metallic- (nach oben)
// und Roughness-Wert (nach rechts) auf einem Boden mit Säulen, Sonne, Schatten und acht
// kreisenden Punktlichtern, oder mit --model ein glTF-Modell. Freie Kamera: rechte Maustaste
// halten und Maus bewegen, W/A/S/D fliegen, E/Q hoch und runter, Shift schneller. F1 schaltet
// Debug-Zeichnen um, F2 färbt die Kaskaden der Schatten ein.
//
// Optionen:
//   --model <pfad>  glTF-Modell (.gltf oder .glb) statt des Kugelrasters
//   --image <pfad>  ohne Fenster offscreen zeichnen und als PPM-Bild speichern (mit Kugelraster,
//                   Modell oder --voxel-demo)
//   --debug-draw    Debug-Zeichnen von Anfang an (Hüllquader, Lichter, Achsenkreuz)
//   --headless      ohne Fenster und GPU starten (wie später der dedizierte Server)
//   --frames <n>    nach n Frames beenden (für automatische Tests)
//   --no-vsync      ohne vertikale Synchronisation präsentieren
//   --baseline      optionale GPU-Stufen (Mesh Shader, Raytracing) abschalten, um die
//                   Rückfallpfade auf starker Hardware zu prüfen
//   --jobs-demo     Job-System und Speicher vorführen (ohne Fenster; mit --frames die Länge)
//   --scene-demo    ECS, Systeme und Szenen vorführen: bewegen, speichern, laden, vergleichen
//   --scene <pfad>  Szenendatei für --scene-demo (sonst im Benutzerordner)
//   --voxel-demo    Voxel-Terrain vorführen: prozedurale Welt aus 12 × 4 × 12 Chunks (Hügel,
//                   Felswände, Höhlen, Überhänge) mit Materialien, Licht und Schatten, mit der
//                   Maus veränderbar, auch über Chunk-Grenzen (links abtragen, rechts auftragen,
//                   Mitte glätten, Rad zoomt, Leertaste hält die Kamera an). F schaltet den
//                   Flugmodus um (rechte Maustaste dreht, W/A/S/D, E/Q, Shift; links abtragen,
//                   Mitte auftragen), F1 zeigt die Chunks. Mit --frames ändert sich das Terrain
//                   automatisch; mit --headless ohne GPU, dann misst die Demo Erzeugen und
//                   Meshing vieler Chunks seriell und parallel.
//   --seed <n>      Startwert der Welt für --voxel-demo (Standard 1)
//   --far           Szene 10 000 km vom Ursprung: zeigt, dass kamerarelatives Zeichnen mit
//                   64-Bit-Positionen nicht zittert
//   --script-demo   C#-Scripting vorführen: .NET starten, Skripte laden, Daten ändern, neu laden
//   --net-demo      Netzwerk vorführen: Server und Client im selben Prozess über localhost
//   --server        dedizierter Server ohne Fenster (Port mit --port, Standard 27020); ohne
//                   --frames läuft er, bis man ihn beendet
//   --client <adr>  ohne Fenster zu einem Server verbinden, z. B. 127.0.0.1 oder host:27020
//   --no-cache      Modelle aus den Quellen laden statt gekocht aus dem Cache (Vergleich, R2.5)

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>

import ngine.core;
import ngine.filesystem;
import ngine.render;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.shaders;
import ngine.profile;
import sandbox.jobs_demo;
import sandbox.scene_demo;
import sandbox.frame_stats;
import sandbox.voxel_demo;
import sandbox.model_demo;
import app.model_scene;
import app.paths;
import ngine.asset;
import sandbox.script_demo;
import sandbox.net_demo;
import ngine.voxel;
#if NGINE_WITH_WINDOW
import ngine.platform;
#endif

namespace {

struct Options {
    bool headless = false;
    bool vsync = true;
    bool jobs_demo = false;
    bool scene_demo = false;
    std::optional<std::string> scene_path;
    bool voxel_demo = false;
    ngine::u32 seed = 1;
    bool script_demo = false;
    bool net_demo = false;
    bool server = false;
    std::optional<ngine::u16> port;
    std::optional<std::string> client_address;
    std::optional<std::string> image_path;
    std::optional<std::string> model_path;
    bool baseline = false;
    bool far = false;
    bool debug_draw = false;
    bool no_cache = false;
    std::optional<ngine::u64> frames;
};

Options parse_options(std::span<char*> args) {
    Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--jobs-demo") {
            options.jobs_demo = true;
        } else if (arg == "--scene-demo") {
            options.scene_demo = true;
        } else if (arg == "--scene" && i + 1 < args.size()) {
            options.scene_path = std::string{args[++i]};
        } else if (arg == "--script-demo") {
            options.script_demo = true;
        } else if (arg == "--net-demo") {
            options.net_demo = true;
        } else if (arg == "--server") {
            options.server = true;
        } else if (arg == "--client" && i + 1 < args.size()) {
            options.client_address = std::string{args[++i]};
        } else if (arg == "--port" && i + 1 < args.size()) {
            const std::string_view value{args[++i]};
            ngine::u16 port = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), port).ec ==
                std::errc{}) {
                options.port = port;
            }
        } else if (arg == "--voxel-demo") {
            options.voxel_demo = true;
        } else if (arg == "--seed" && i + 1 < args.size()) {
            const std::string_view value{args[++i]};
            ngine::u32 seed = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), seed).ec ==
                std::errc{}) {
                options.seed = seed;
            }
        } else if (arg == "--model" && i + 1 < args.size()) {
            options.model_path = std::string{args[++i]};
        } else if (arg == "--image" && i + 1 < args.size()) {
            options.image_path = std::string{args[++i]};
        } else if (arg == "--no-vsync") {
            options.vsync = false;
        } else if (arg == "--baseline") {
            options.baseline = true;
        } else if (arg == "--far") {
            options.far = true;
        } else if (arg == "--debug-draw") {
            options.debug_draw = true;
        } else if (arg == "--no-cache") {
            options.no_cache = true;
        } else if (arg == "--frames" && i + 1 < args.size()) {
            const std::string_view value{args[++i]};
            ngine::u64 frames = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), frames).ec ==
                std::errc{}) {
                options.frames = frames;
            }
        } else {
            ngine::log::warn("Unbekannte Option: {}", arg);
        }
    }
    return options;
}

#if NGINE_WITH_WINDOW

int run_windowed(const Options& options) {
    auto window = ngine::platform::Window::create({.title = "NGine Sandbox"});
    if (!window) {
        ngine::log::fatal("{}", window.error());
        return 1;
    }

    const auto size = window->framebuffer_size();
    ngine::rhi::DeviceDesc device_desc{
        .application_name = "NGine Sandbox",
#ifdef NDEBUG
        .enable_validation = false,
#else
        .enable_validation = true,
#endif
        .vsync = options.vsync,
        .surface =
            ngine::rhi::SurfaceDesc{
                .instance_extensions = window->vulkan_instance_extensions(),
                .create_surface =
                    [&](void* instance) { return window->create_vulkan_surface(instance); },
                .extent = {size.width, size.height},
            },
        .disabled_features = {.mesh_shader = options.baseline, .ray_tracing = options.baseline},
    };
    auto device = ngine::rhi::vulkan::create_device(device_desc);
    if (!device) {
        ngine::log::fatal("{}", device.error());
        return 1;
    }

    int exit_code = 0;
    {
        // Ressourcen leben in diesem Block, damit sie vor dem Device zerstört werden.
        // Voxel-Demo (Durchstich D1) oder Standardansicht mit Renderer und Welt (R1.3).
        std::optional<sandbox::VoxelScene> voxels;
        // Asset-Ordner und Cache leben länger als die Szene.
        sandbox::SandboxAssets sandbox_assets{options.no_cache};
        std::unique_ptr<app::ModelScene> scene;
        if (options.voxel_demo) {
            voxels.emplace(sandbox::voxel_origin(options.far), sandbox::demo_terrain(options.seed));
            if (auto result = voxels->create_gpu(**device, (*device)->swapchain_format(),
                                                 sandbox_assets.database());
                !result) {
                ngine::log::fatal("Voxel-Demo: {}", result.error());
                return 1;
            }
        } else {
            auto created =
                app::ModelScene::create(**device, (*device)->swapchain_format(), options.model_path,
                                        sandbox::voxel_origin(options.far), app::SceneMode::Game,
                                        sandbox_assets.scene_assets());
            if (!created) {
                ngine::log::fatal("{}", created.error());
                return 1;
            }
            scene = *std::move(created);
            scene->set_debug_draw(options.debug_draw);
            sandbox::report_load(scene->load_all(**device));
        }

        ngine::render::TexturePool pool{**device};
        // GPU-Zonen pro Pass in Tracy (CORE-07).
        ngine::render::GpuProfiler gpu_profiler{**device};
        ngine::u64 frame_count = 0;
        bool running = true;
        app::FlyInput fly;
        bool looking = false;
        auto last_time = std::chrono::steady_clock::now();
        auto last_report = last_time;

        // Ein ganzer Frame: Zeit, Szene, Zeichnen. Läuft aus der Hauptschleife und, unter
        // Windows, aus dem Rückruf beim Ziehen des Fensters (TD-19).
        const auto run_frame = [&] {
            const auto now = std::chrono::steady_clock::now();
            const auto dt = std::chrono::duration<float>(now - last_time).count();
            last_time = now;
            if (voxels) {
                const auto extent = window->framebuffer_size();
                voxels->update(dt, {extent.width, extent.height}, fly);
                fly.look_x = 0.0f;
                fly.look_y = 0.0f;
                // Automatische Änderungen, wenn die Laufzeit begrenzt ist (CI).
                if (options.frames && frame_count % 10 == 0) {
                    voxels->scripted_edit(frame_count / 10);
                }
                voxels->remesh_if_needed();
            } else {
                scene->update(dt, fly);
                fly.look_x = 0.0f;
                fly.look_y = 0.0f;
            }
            if (now - last_report > std::chrono::seconds{3}) {
                ngine::log::info("{}", voxels ? "Voxel-Terrain: " + voxels->stats()
                                              : "Szene: " + scene->stats());
                last_report = now;
            }

            if (auto frame = (*device)->begin_frame()) {
                gpu_profiler.begin_frame(*frame);
                // Jeder Frame ist ein Render-Graph; er setzt alle Barrieren, auch die für die
                // Präsentation am Ende.
                ngine::render::RenderGraph graph;
                const auto backbuffer = graph.import_texture("Backbuffer", frame->backbuffer,
                                                             ngine::rhi::ResourceState::Undefined,
                                                             ngine::rhi::ResourceState::Present);
                if (voxels) {
                    voxels->add_passes(**device, graph, backbuffer);
                } else {
                    scene->add_passes(graph, backbuffer);
                }
                graph.execute(frame->commands, pool, &gpu_profiler);
                pool.end_frame();
                (*device)->end_frame();
                ++frame_count;
            } else {
                // Minimiert oder Swapchain wird neu aufgebaut: nicht im Leerlauf kreiseln.
                std::this_thread::sleep_for(std::chrono::milliseconds{16});
            }
            sandbox::report_memory();
            ngine::profile::frame_mark();
        };
        window->set_redraw_callback([&](ngine::platform::Extent extent) {
            (*device)->resize({extent.width, extent.height});
            run_frame();
        });

        while (running) {
            namespace event = ngine::platform::event;
            using ngine::platform::Key;
            const auto set_key = [&](Key key, bool down) {
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
            for (const auto& e : window->poll_events()) {
                std::visit(
                    [&](const auto& ev) {
                        using T = std::decay_t<decltype(ev)>;
                        using ngine::platform::MouseButton;
                        if constexpr (std::is_same_v<T, event::Quit>) {
                            running = false;
                        } else if constexpr (std::is_same_v<T, event::Resized>) {
                            (*device)->resize({ev.framebuffer.width, ev.framebuffer.height});
                        } else if constexpr (std::is_same_v<T, event::KeyPressed>) {
                            if (ev.key == Key::Escape) {
                                running = false;
                            } else if (ev.key == Key::Space && voxels) {
                                voxels->toggle_rotation();
                            } else if (ev.key == Key::F && voxels) {
                                voxels->toggle_fly();
                            } else if (ev.key == Key::F1 && voxels) {
                                voxels->toggle_chunk_bounds();
                            } else if (ev.key == Key::F1 && scene) {
                                scene->toggle_debug_draw();
                            } else if (ev.key == Key::F2 && scene) {
                                scene->toggle_cascades();
                            }
                            set_key(ev.key, true);
                        } else if constexpr (std::is_same_v<T, event::KeyReleased>) {
                            set_key(ev.key, false);
                        } else if constexpr (std::is_same_v<T, event::MouseMoved>) {
                            if (voxels) {
                                voxels->set_cursor(ev.x, ev.y);
                            }
                            if (looking) {
                                fly.look_x += ev.dx;
                                fly.look_y += ev.dy;
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseButtonPressed>) {
                            const bool look_button =
                                ev.button == MouseButton::Right && (!voxels || voxels->flying());
                            if (voxels && !look_button) {
                                using ngine::voxel::BrushMode;
                                voxels->set_cursor(ev.x, ev.y);
                                // Im Flugmodus dreht die rechte Taste; die mittlere trägt auf.
                                const BrushMode middle =
                                    voxels->flying() ? BrushMode::Add : BrushMode::Smooth;
                                voxels->set_brush(ev.button == MouseButton::Left ? BrushMode::Remove
                                                  : ev.button == MouseButton::Right ? BrushMode::Add
                                                                                    : middle);
                            } else if (look_button) {
                                looking = true;
                                window->set_relative_mouse(true);
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseButtonReleased>) {
                            if (voxels && (ev.button != MouseButton::Right || !looking)) {
                                voxels->set_brush(std::nullopt);
                            } else if (ev.button == MouseButton::Right) {
                                looking = false;
                                window->set_relative_mouse(false);
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseWheel>) {
                            if (voxels) {
                                voxels->zoom(ev.delta);
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
        if (voxels) {
            ngine::log::info("Voxel-Terrain: {}", voxels->stats());
            voxels->release_gpu();
        } else {
            ngine::log::info("Szene: {}", scene->stats());
        }
        ngine::log::info("{} Frames gezeichnet", frame_count);
    }

    if (const auto errors = (*device)->validation_errors(); errors > 0) {
        ngine::log::error("Die Validation Layers haben {} Fehler gemeldet", errors);
        exit_code = 1;
    }
    return exit_code;
}

#endif

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(std::span{argv, static_cast<std::size_t>(argc)});

    const auto version = ngine::engine_version;
    ngine::log::info("{} {}.{}.{} startet", ngine::engine_name, version.major, version.minor,
                     version.patch);

    int exit_code = 0;
    if (options.jobs_demo) {
        exit_code = sandbox::run_jobs_demo(options.frames.value_or(120));
    } else if (options.script_demo) {
        // Die Assemblies liegen im Ordner „dotnet“ neben der Sandbox (TD-12).
        std::string dotnet_dir;
#if NGINE_WITH_DOTNET
        if (auto exe = ngine::filesystem::executable_dir()) {
            dotnet_dir = ngine::filesystem::join(*exe, "dotnet");
        }
#endif
        exit_code = sandbox::run_script_demo(options.frames.value_or(120), dotnet_dir);
    } else if (options.net_demo) {
        exit_code = sandbox::run_net_demo(options.frames.value_or(180));
    } else if (options.server) {
        exit_code = sandbox::run_server(options.frames, options.port);
    } else if (options.client_address) {
        exit_code = sandbox::run_client(*options.client_address, options.frames.value_or(300));
    } else if (options.voxel_demo && options.headless) {
        exit_code = sandbox::run_voxel_demo_headless(options.frames.value_or(120), options.seed);
    } else if (options.voxel_demo && options.image_path) {
        const sandbox::SandboxAssets sandbox_assets{options.no_cache};
        exit_code = sandbox::run_voxel_demo_offscreen(
            options.frames.value_or(60), *options.image_path, options.baseline, options.far,
            options.seed, sandbox_assets.database());
    } else if (options.image_path && !options.voxel_demo) {
        exit_code = sandbox::run_model_offscreen(
            options.model_path, *options.image_path, options.baseline,
            sandbox::voxel_origin(options.far), options.debug_draw, options.no_cache);
    } else if (options.scene_demo) {
        exit_code = sandbox::run_scene_demo(options.frames.value_or(120), options.scene_path);
    } else if (options.headless) {
        ngine::log::info("Modus: ohne Fenster (Server)");
    } else {
#if NGINE_WITH_WINDOW
        exit_code = run_windowed(options);
#else
        ngine::log::error("Diese Sandbox wurde ohne Fenster gebaut (NGINE_WITH_WINDOW=OFF)");
        exit_code = 1;
#endif
    }

    ngine::log::info("Beendet");
    ngine::log::flush();
    return exit_code;
}
