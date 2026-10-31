// Sandbox: kleine Testanwendung, an der jede Iteration sichtbar wird.
//
// Optionen:
//   --headless      ohne Fenster und GPU starten (wie später der dedizierte Server)
//   --frames <n>    nach n Frames beenden (für automatische Tests)
//   --no-vsync      ohne vertikale Synchronisation präsentieren
//   --jobs-demo     Job-System und Speicher vorführen (ohne Fenster; mit --frames die Länge)

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
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
#if NGINE_WITH_WINDOW
import ngine.platform;
#endif

namespace {

struct Options {
    bool headless = false;
    bool vsync = true;
    bool jobs_demo = false;
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
        } else if (arg == "--no-vsync") {
            options.vsync = false;
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

        ngine::u64 frame_count = 0;
        bool running = true;
        while (running) {
            for (const auto& event : window->poll_events()) {
                std::visit(
                    [&](const auto& e) {
                        using T = std::decay_t<decltype(e)>;
                        if constexpr (std::is_same_v<T, ngine::platform::event::Quit>) {
                            running = false;
                        } else if constexpr (std::is_same_v<T, ngine::platform::event::Resized>) {
                            (*device)->resize({e.framebuffer.width, e.framebuffer.height});
                        } else if constexpr (std::is_same_v<T,
                                                            ngine::platform::event::KeyPressed>) {
                            if (e.key == ngine::platform::Key::Escape) {
                                running = false;
                            }
                        }
                    },
                    event);
            }

            if (auto frame = (*device)->begin_frame()) {
                frame->commands.begin_rendering(frame->backbuffer,
                                                ngine::rhi::Color{0.02f, 0.02f, 0.04f, 1.0f});
                frame->commands.bind_pipeline(**pipeline);
                frame->commands.draw(3);
                frame->commands.end_rendering();
                (*device)->end_frame();
                ++frame_count;
            } else {
                // Minimiert oder Swapchain wird neu aufgebaut: nicht im Leerlauf kreiseln.
                std::this_thread::sleep_for(std::chrono::milliseconds{16});
            }

            ngine::profile::frame_mark();
            if (options.frames && frame_count >= *options.frames) {
                running = false;
            }
        }

        (*device)->wait_idle();
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
