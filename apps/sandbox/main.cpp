// Sandbox: kleine Testanwendung, an der jede Iteration sichtbar wird.
//
// Optionen:
//   --headless      ohne Fenster und GPU starten (wie später der dedizierte Server)
//   --frames <n>    nach n Frames beenden (für automatische Tests)
//   --no-vsync      ohne vertikale Synchronisation präsentieren
//   --baseline      optionale GPU-Stufen (Mesh Shader, Raytracing) abschalten, um die
//                   Rückfallpfade auf starker Hardware zu prüfen
//   --jobs-demo     Job-System und Speicher vorführen (ohne Fenster; mit --frames die Länge)
//   --scene-demo    ECS, Systeme und Szenen vorführen: bewegen, speichern, laden, vergleichen
//   --scene <pfad>  Szenendatei für --scene-demo (sonst im Benutzerordner)
//   --voxel-demo    Voxel-Terrain vorführen: Hügel aus Surface Nets, mit der Maus veränderbar
//                   (links abtragen, rechts auftragen, Mitte glätten, Rad zoomt, Leertaste hält
//                   die Kamera an). Mit --frames ändert sich das Terrain automatisch; mit
//                   --headless ohne GPU, mit --image <pfad> offscreen als PPM-Bild.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>

import ngine.core;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.shaders;
import ngine.profile;
import sandbox.jobs_demo;
import sandbox.scene_demo;
import sandbox.frame_stats;
import sandbox.voxel_demo;
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
    std::optional<std::string> image_path;
    bool baseline = false;
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
        } else if (arg == "--voxel-demo") {
            options.voxel_demo = true;
        } else if (arg == "--image" && i + 1 < args.size()) {
            options.image_path = std::string{args[++i]};
        } else if (arg == "--no-vsync") {
            options.vsync = false;
        } else if (arg == "--baseline") {
            options.baseline = true;
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
        auto vertex = (*device)->create_shader(ngine::shaders::triangle_vertex());
        auto fragment = (*device)->create_shader(ngine::shaders::triangle_fragment());
        if (!vertex || !fragment) {
            ngine::log::fatal("Shader: {}", !vertex ? vertex.error() : fragment.error());
            return 1;
        }
        auto pipeline = (*device)->create_pipeline({
            .vertex = vertex->get(),
            .fragment = fragment->get(),
            .color_format = (*device)->swapchain_format(),
        });
        if (!pipeline) {
            ngine::log::fatal("Pipeline: {}", pipeline.error());
            return 1;
        }

        // Voxel-Demo statt Dreieck (Durchstich D1).
        std::optional<sandbox::VoxelScene> voxels;
        if (options.voxel_demo) {
            voxels.emplace();
            if (auto result = voxels->create_gpu(**device, (*device)->swapchain_format());
                !result) {
                ngine::log::fatal("Voxel-Demo: {}", result.error());
                return 1;
            }
        }

        ngine::u64 frame_count = 0;
        bool running = true;
        auto last_time = std::chrono::steady_clock::now();
        auto last_report = last_time;
        while (running) {
            namespace event = ngine::platform::event;
            for (const auto& e : window->poll_events()) {
                std::visit(
                    [&](const auto& ev) {
                        using T = std::decay_t<decltype(ev)>;
                        if constexpr (std::is_same_v<T, event::Quit>) {
                            running = false;
                        } else if constexpr (std::is_same_v<T, event::Resized>) {
                            (*device)->resize({ev.framebuffer.width, ev.framebuffer.height});
                        } else if constexpr (std::is_same_v<T, event::KeyPressed>) {
                            if (ev.key == ngine::platform::Key::Escape) {
                                running = false;
                            } else if (ev.key == ngine::platform::Key::Space && voxels) {
                                voxels->toggle_rotation();
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseMoved>) {
                            if (voxels) {
                                voxels->set_cursor(ev.x, ev.y);
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseButtonPressed>) {
                            if (voxels) {
                                using ngine::platform::MouseButton;
                                using ngine::voxel::BrushMode;
                                voxels->set_cursor(ev.x, ev.y);
                                voxels->set_brush(ev.button == MouseButton::Left ? BrushMode::Remove
                                                  : ev.button == MouseButton::Right
                                                      ? BrushMode::Add
                                                      : BrushMode::Smooth);
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseButtonReleased>) {
                            if (voxels) {
                                voxels->set_brush(std::nullopt);
                            }
                        } else if constexpr (std::is_same_v<T, event::MouseWheel>) {
                            if (voxels) {
                                voxels->zoom(ev.delta);
                            }
                        }
                    },
                    e);
            }

            const auto now = std::chrono::steady_clock::now();
            const auto dt = std::chrono::duration<float>(now - last_time).count();
            last_time = now;
            if (voxels) {
                const auto size = window->framebuffer_size();
                voxels->update(dt, {size.width, size.height});
                // Automatische Änderungen, wenn die Laufzeit begrenzt ist (CI).
                if (options.frames && frame_count % 10 == 0) {
                    voxels->scripted_edit(frame_count / 10);
                }
                voxels->remesh_if_needed();
                if (now - last_report > std::chrono::seconds{3}) {
                    ngine::log::info("Voxel-Netz: {}", voxels->stats());
                    last_report = now;
                }
            }

            if (auto frame = (*device)->begin_frame()) {
                if (voxels) {
                    voxels->record(**device, frame->commands, frame->backbuffer, frame->index);
                } else {
                    frame->commands.begin_rendering(frame->backbuffer,
                                                    ngine::rhi::Color{0.02f, 0.02f, 0.04f, 1.0f});
                    frame->commands.bind_pipeline(**pipeline);
                    frame->commands.draw(3);
                    frame->commands.end_rendering();
                }
                (*device)->end_frame();
                ++frame_count;
            } else {
                // Minimiert oder Swapchain wird neu aufgebaut: nicht im Leerlauf kreiseln.
                std::this_thread::sleep_for(std::chrono::milliseconds{16});
            }

            sandbox::report_memory();
            ngine::profile::frame_mark();
            if (options.frames && frame_count >= *options.frames) {
                running = false;
            }
        }

        (*device)->wait_idle();
        if (voxels) {
            ngine::log::info("Voxel-Netz: {}", voxels->stats());
            voxels->release_gpu();
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
    } else if (options.voxel_demo && options.headless) {
        exit_code = sandbox::run_voxel_demo_headless(options.frames.value_or(120));
    } else if (options.voxel_demo && options.image_path) {
        exit_code = sandbox::run_voxel_demo_offscreen(options.frames.value_or(60),
                                                      *options.image_path, options.baseline);
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
