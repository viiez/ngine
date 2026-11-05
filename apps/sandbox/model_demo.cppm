module;

#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Standardansicht der Sandbox offscreen als Bild (--image). Die Szene selbst liegt seit R2.1 in
// apps/common (app.model_scene), weil auch der Editor sie zeigt.
export module sandbox.model_demo;

import ngine.core;
import ngine.filesystem;
import ngine.math;
import ngine.render;
import ngine.rhi;
import ngine.rhi.vulkan;
import app.model_scene;

namespace sandbox {

using namespace ngine;
using app::ModelScene;
using math::DVec3;

namespace {

rhi::Result<void> write_ppm(const std::string& path, std::span<const u8> rgba,
                            rhi::Extent2D extent) {
    const std::string header = std::format("P6\n{} {}\n255\n", extent.width, extent.height);
    std::vector<std::byte> data;
    data.reserve(header.size() + usize{extent.width} * extent.height * 3);
    for (const char c : header) {
        data.push_back(static_cast<std::byte>(c));
    }
    for (usize i = 0; i + 3 < rgba.size(); i += 4) {
        data.push_back(static_cast<std::byte>(rgba[i]));
        data.push_back(static_cast<std::byte>(rgba[i + 1]));
        data.push_back(static_cast<std::byte>(rgba[i + 2]));
    }
    return filesystem::write_file(path, data);
}

} // namespace

// Offscreen auf der GPU zeichnen und als PPM speichern (für die CI): Kugelraster oder Modell.
export int run_model_offscreen(const std::optional<std::string>& model_path,
                               const std::string& image_path, bool baseline, DVec3 origin,
                               bool debug_draw) {
    auto device = rhi::vulkan::create_device({
        .application_name = "NGine Sandbox",
#ifdef NDEBUG
        .enable_validation = false,
#else
        .enable_validation = true,
#endif
        .disabled_features = {.mesh_shader = baseline, .ray_tracing = baseline},
    });
    if (!device) {
        log::fatal("{}", device.error());
        return 1;
    }
    int exit_code = 0;
    {
        constexpr rhi::Extent2D extent{960, 540};
        constexpr rhi::Format format = rhi::Format::RGBA8Srgb;
        auto target = (*device)->create_texture({.extent = extent, .format = format});
        if (!target) {
            log::fatal("{}", target.error());
            return 1;
        }
        auto scene = ModelScene::create(**device, format, model_path, origin);
        if (!scene) {
            log::fatal("{}", scene.error());
            return 1;
        }
        (*scene)->set_debug_draw(debug_draw);
        render::TexturePool pool{**device};
        render::RenderGraph graph;
        const auto output = graph.import_texture("Bild", **target, rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::TransferSrc);
        (*scene)->add_passes(graph, output);
        (*device)->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
        log::info("Szene: {}", (*scene)->stats());
        const auto pixels = (*device)->read_texture(**target, rhi::ResourceState::TransferSrc);
        if (auto written = write_ppm(image_path, pixels, extent); !written) {
            log::error("Bild lässt sich nicht schreiben: {}", written.error());
            exit_code = 1;
        } else {
            log::info("Bild gespeichert: {}", image_path);
        }
    }
    if (const auto errors = (*device)->validation_errors(); errors > 0) {
        log::error("Die Validation Layers haben {} Fehler gemeldet", errors);
        exit_code = 1;
    }
    return exit_code;
}

} // namespace sandbox
