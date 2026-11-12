module;

#include <ngine/profile.hpp>

#include <array>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shader und Pipelines des Renderers (TD-36): eingebettetes SPIR-V beim Start, neu übersetzt beim
// Hot Reload. Seit R3.5 auch Compute-Pipelines (Prüfen, Hi-Z) und, wenn die GPU Mesh Shader kann,
// Varianten mit Task- und Mesh-Shader.
module ngine.render;

import ngine.core;
import ngine.profile;
import ngine.rhi;
import ngine.shaders;
import :renderer_detail;

namespace ngine::render {

using detail::depth_format;
using detail::hdr_format;
using detail::id_format;

std::span<const RendererCore::ShaderSpec> RendererCore::shader_specs() {
    static constexpr std::array specs{
        ShaderSpec{"pbr.slang", shaders::pbr_vertex, &RendererCore::pbr_vertex},
        ShaderSpec{"pbr.slang", shaders::pbr_fragment, &RendererCore::pbr_fragment},
        ShaderSpec{"shadow.slang", shaders::shadow_vertex, &RendererCore::shadow_vertex},
        ShaderSpec{"shadow.slang", shaders::shadow_fragment, &RendererCore::shadow_fragment},
        ShaderSpec{"debug.slang", shaders::debug_vertex, &RendererCore::debug_vertex},
        ShaderSpec{"debug.slang", shaders::debug_fragment, &RendererCore::debug_fragment},
        ShaderSpec{"fullscreen.slang", shaders::fullscreen_vertex,
                   &RendererCore::fullscreen_vertex},
        ShaderSpec{"tonemap.slang", shaders::tonemap_fragment, &RendererCore::tonemap_fragment},
        ShaderSpec{"id.slang", shaders::id_vertex, &RendererCore::id_vertex},
        ShaderSpec{"id.slang", shaders::id_fragment, &RendererCore::id_fragment},
        ShaderSpec{"terrain.slang", shaders::terrain_vertex, &RendererCore::terrain_vertex},
        ShaderSpec{"terrain.slang", shaders::terrain_fragment, &RendererCore::terrain_fragment},
        ShaderSpec{"terrain.slang", shaders::terrain_shadow_vertex,
                   &RendererCore::terrain_shadow_vertex},
        ShaderSpec{"terrain.slang", shaders::terrain_id_vertex, &RendererCore::terrain_id_vertex},
        ShaderSpec{"terrain.slang", shaders::terrain_id_fragment,
                   &RendererCore::terrain_id_fragment},
        ShaderSpec{"cull.slang", shaders::cull_compute, &RendererCore::cull_compute},
        ShaderSpec{"hiz.slang", shaders::hiz_compute, &RendererCore::hiz_compute},
        ShaderSpec{"cull.slang", shaders::cull_task, &RendererCore::cull_task, true},
        ShaderSpec{"pbr.slang", shaders::pbr_mesh, &RendererCore::pbr_mesh, true},
        ShaderSpec{"shadow.slang", shaders::shadow_mesh, &RendererCore::shadow_mesh, true},
        ShaderSpec{"id.slang", shaders::id_mesh, &RendererCore::id_mesh, true},
        ShaderSpec{"terrain.slang", shaders::terrain_mesh, &RendererCore::terrain_mesh, true},
        ShaderSpec{"terrain.slang", shaders::terrain_shadow_mesh,
                   &RendererCore::terrain_shadow_mesh, true},
        ShaderSpec{"terrain.slang", shaders::terrain_id_mesh, &RendererCore::terrain_id_mesh, true},
    };
    return specs;
}

bool RendererCore::mesh_capable() const {
    return device->capabilities().features.mesh_shader;
}

rhi::Result<void> RendererCore::create_shaders() {
    for (const ShaderSpec& spec : shader_specs()) {
        // Ohne die Stufe nimmt die GPU kein SPIR-V für Task- und Mesh-Shader.
        if (spec.mesh && !mesh_capable()) {
            continue;
        }
        auto created = device->create_shader(spec.embedded());
        if (!created) {
            return std::unexpected("Renderer, Shader: " + created.error());
        }
        this->*spec.slot = *std::move(created);
    }
    return {};
}

rhi::Result<void> RendererCore::reload_shaders(const ShaderSource& source) {
    NGINE_PROFILE_ZONE("Renderer::reload_shaders");
    const std::span<const ShaderSpec> specs = shader_specs();
    // Erst alles übersetzen; ein Fehler lässt die laufenden Shader stehen.
    std::vector<std::unique_ptr<rhi::Shader>> compiled;
    for (const ShaderSpec& spec : specs) {
        if (spec.mesh && !mesh_capable()) {
            compiled.emplace_back();
            continue;
        }
        const rhi::ShaderDesc embedded = spec.embedded();
        auto spirv = source(spec.file, embedded.entry_point, embedded.stage);
        if (!spirv) {
            return std::unexpected(
                std::format("{} ({}): {}", spec.file, embedded.entry_point, spirv.error()));
        }
        auto shader = device->create_shader(
            {.stage = embedded.stage, .spirv = *spirv, .entry_point = embedded.entry_point});
        if (!shader) {
            return std::unexpected(
                std::format("{} ({}): {}", spec.file, embedded.entry_point, shader.error()));
        }
        compiled.push_back(*std::move(shader));
    }
    // Übersprungene Shader (Mesh Shader ohne die Stufe) bleiben leer.
    std::vector<std::unique_ptr<rhi::Shader>> previous;
    for (usize i = 0; i < compiled.size(); ++i) {
        previous.push_back(std::exchange(this->*specs[i].slot, std::move(compiled[i])));
    }
    if (auto result = create_pipelines(); !result) {
        // Mit den alten Shadern weiter.
        for (usize i = 0; i < previous.size(); ++i) {
            this->*specs[i].slot = std::move(previous[i]);
        }
        static_cast<void>(create_pipelines());
        return result;
    }
    return {};
}

rhi::Result<void> RendererCore::create_pipelines() {
    for (usize i = 0; i < pbr.size(); ++i) {
        const bool blend = i >= 2;
        auto pipeline = device->create_pipeline({
            .vertex = pbr_vertex.get(),
            .fragment = pbr_fragment.get(),
            .color_format = hdr_format,
            .depth_format = depth_format,
            // Umgekehrte Tiefe: näher heißt größer.
            .depth_compare = rhi::CompareOp::Greater,
            .depth_write = !blend,
            .cull_mode = (i % 2 == 1) ? rhi::CullMode::None : rhi::CullMode::Back,
            .blend = blend ? rhi::BlendMode::Alpha : rhi::BlendMode::None,
        });
        if (!pipeline) {
            return std::unexpected("Renderer, PBR-Pipeline: " + pipeline.error());
        }
        pbr[i] = *std::move(pipeline);
    }
    const bool meshlets = mesh_capable();
    for (usize i = 0; meshlets && i < pbr_meshlets.size(); ++i) {
        auto pipeline = device->create_pipeline({
            .task = cull_task.get(),
            .mesh = pbr_mesh.get(),
            .fragment = pbr_fragment.get(),
            .color_format = hdr_format,
            .depth_format = depth_format,
            .depth_compare = rhi::CompareOp::Greater,
            .cull_mode = i == 1 ? rhi::CullMode::None : rhi::CullMode::Back,
        });
        if (!pipeline) {
            return std::unexpected("Renderer, PBR-Pipeline (Mesh Shader): " + pipeline.error());
        }
        pbr_meshlets[i] = *std::move(pipeline);
    }
    if (auto result = create_shadow_pipelines(shadow_bias_constant, shadow_bias_slope); !result) {
        return result;
    }
    auto lines_pipeline = device->create_pipeline({.vertex = debug_vertex.get(),
                                                   .fragment = debug_fragment.get(),
                                                   .color_format = hdr_format,
                                                   .depth_format = depth_format,
                                                   .depth_compare = rhi::CompareOp::GreaterOrEqual,
                                                   .depth_write = false});
    if (!lines_pipeline) {
        return std::unexpected("Renderer, Debug-Linien: " + lines_pipeline.error());
    }
    debug_lines = *std::move(lines_pipeline);
    auto tonemap_pipeline = device->create_pipeline({.vertex = fullscreen_vertex.get(),
                                                     .fragment = tonemap_fragment.get(),
                                                     .color_format = target_format});
    if (!tonemap_pipeline) {
        return std::unexpected("Renderer, Tone Mapping: " + tonemap_pipeline.error());
    }
    tonemap = *std::move(tonemap_pipeline);
    for (usize i = 0; i < id_pipelines.size(); ++i) {
        auto pipeline = device->create_pipeline(
            {.vertex = id_vertex.get(),
             .fragment = id_fragment.get(),
             .color_format = id_format,
             .depth_format = depth_format,
             .depth_compare = rhi::CompareOp::Greater,
             .cull_mode = i == 0 ? rhi::CullMode::Back : rhi::CullMode::None});
        if (!pipeline) {
            return std::unexpected("Renderer, ID-Puffer: " + pipeline.error());
        }
        id_pipelines[i] = *std::move(pipeline);
    }
    for (usize i = 0; meshlets && i < id_meshlets.size(); ++i) {
        auto pipeline = device->create_pipeline(
            {.task = cull_task.get(),
             .mesh = id_mesh.get(),
             .fragment = id_fragment.get(),
             .color_format = id_format,
             .depth_format = depth_format,
             .depth_compare = rhi::CompareOp::Greater,
             .cull_mode = i == 0 ? rhi::CullMode::Back : rhi::CullMode::None});
        if (!pipeline) {
            return std::unexpected("Renderer, ID-Puffer (Mesh Shader): " + pipeline.error());
        }
        id_meshlets[i] = *std::move(pipeline);
    }
    auto terrain_pipeline = device->create_pipeline({.vertex = terrain_vertex.get(),
                                                     .fragment = terrain_fragment.get(),
                                                     .color_format = hdr_format,
                                                     .depth_format = depth_format,
                                                     .depth_compare = rhi::CompareOp::Greater,
                                                     .cull_mode = rhi::CullMode::Back});
    if (!terrain_pipeline) {
        return std::unexpected("Renderer, Terrain: " + terrain_pipeline.error());
    }
    terrain = *std::move(terrain_pipeline);
    auto terrain_id_pipeline = device->create_pipeline({.vertex = terrain_id_vertex.get(),
                                                        .fragment = terrain_id_fragment.get(),
                                                        .color_format = id_format,
                                                        .depth_format = depth_format,
                                                        .depth_compare = rhi::CompareOp::Greater,
                                                        .cull_mode = rhi::CullMode::Back});
    if (!terrain_id_pipeline) {
        return std::unexpected("Renderer, ID-Puffer des Terrains: " + terrain_id_pipeline.error());
    }
    terrain_id = *std::move(terrain_id_pipeline);
    if (meshlets) {
        auto forward = device->create_pipeline({.task = cull_task.get(),
                                                .mesh = terrain_mesh.get(),
                                                .fragment = terrain_fragment.get(),
                                                .color_format = hdr_format,
                                                .depth_format = depth_format,
                                                .depth_compare = rhi::CompareOp::Greater,
                                                .cull_mode = rhi::CullMode::Back});
        auto ids = device->create_pipeline({.task = cull_task.get(),
                                            .mesh = terrain_id_mesh.get(),
                                            .fragment = terrain_id_fragment.get(),
                                            .color_format = id_format,
                                            .depth_format = depth_format,
                                            .depth_compare = rhi::CompareOp::Greater,
                                            .cull_mode = rhi::CullMode::Back});
        if (!forward || !ids) {
            return std::unexpected("Renderer, Terrain (Mesh Shader): " +
                                   (!forward ? forward.error() : ids.error()));
        }
        terrain_meshlets = *std::move(forward);
        terrain_id_meshlets = *std::move(ids);
    }
    auto cull_pipeline = device->create_compute_pipeline({.compute = cull_compute.get()});
    if (!cull_pipeline) {
        return std::unexpected("Renderer, Prüfen der Meshlets: " + cull_pipeline.error());
    }
    cull = *std::move(cull_pipeline);
    auto hiz_pipeline = device->create_compute_pipeline({.compute = hiz_compute.get()});
    if (!hiz_pipeline) {
        return std::unexpected("Renderer, Hi-Z: " + hiz_pipeline.error());
    }
    hiz = *std::move(hiz_pipeline);
    return {};
}

rhi::Result<void> RendererCore::create_shadow_pipelines(f32 constant, f32 slope) {
    for (const bool masked : {false, true}) {
        auto pipeline = device->create_pipeline({
            .vertex = shadow_vertex.get(),
            .fragment = masked ? shadow_fragment.get() : nullptr,
            .depth_format = depth_format,
            .depth_compare = rhi::CompareOp::LessOrEqual,
            .depth_bias_constant = constant,
            .depth_bias_slope = slope,
            // Ohne Aussortieren werfen auch einseitige Flächen von hinten Schatten.
            .cull_mode = rhi::CullMode::None,
        });
        if (!pipeline) {
            return std::unexpected("Renderer, Schatten-Pipeline: " + pipeline.error());
        }
        (masked ? shadow_masked : shadow_opaque) = *std::move(pipeline);
        if (!mesh_capable()) {
            continue;
        }
        auto meshlets = device->create_pipeline({
            .task = cull_task.get(),
            .mesh = shadow_mesh.get(),
            .fragment = masked ? shadow_fragment.get() : nullptr,
            .depth_format = depth_format,
            .depth_compare = rhi::CompareOp::LessOrEqual,
            .depth_bias_constant = constant,
            .depth_bias_slope = slope,
            .cull_mode = rhi::CullMode::None,
        });
        if (!meshlets) {
            return std::unexpected("Renderer, Schatten-Pipeline (Mesh Shader): " +
                                   meshlets.error());
        }
        (masked ? shadow_masked_meshlets : shadow_opaque_meshlets) = *std::move(meshlets);
    }
    auto terrain_pipeline = device->create_pipeline({
        .vertex = terrain_shadow_vertex.get(),
        .depth_format = depth_format,
        .depth_compare = rhi::CompareOp::LessOrEqual,
        .depth_bias_constant = constant,
        .depth_bias_slope = slope,
        .cull_mode = rhi::CullMode::None,
    });
    if (!terrain_pipeline) {
        return std::unexpected("Renderer, Schatten des Terrains: " + terrain_pipeline.error());
    }
    terrain_shadow = *std::move(terrain_pipeline);
    if (mesh_capable()) {
        auto meshlets = device->create_pipeline({
            .task = cull_task.get(),
            .mesh = terrain_shadow_mesh.get(),
            .depth_format = depth_format,
            .depth_compare = rhi::CompareOp::LessOrEqual,
            .depth_bias_constant = constant,
            .depth_bias_slope = slope,
            .cull_mode = rhi::CullMode::None,
        });
        if (!meshlets) {
            return std::unexpected("Renderer, Schatten des Terrains (Mesh Shader): " +
                                   meshlets.error());
        }
        terrain_shadow_meshlets = *std::move(meshlets);
    }
    shadow_bias_constant = constant;
    shadow_bias_slope = slope;
    return {};
}

detail::CategoryPipelines RendererCore::forward_pipelines(bool mesh_shaders) const {
    if (mesh_shaders) {
        return {terrain_meshlets.get(), pbr_meshlets[0].get(), pbr_meshlets[0].get(),
                pbr_meshlets[1].get(), pbr_meshlets[1].get()};
    }
    return {terrain.get(), pbr[0].get(), pbr[0].get(), pbr[1].get(), pbr[1].get()};
}

detail::CategoryPipelines RendererCore::shadow_pipelines(bool mesh_shaders) const {
    if (mesh_shaders) {
        return {terrain_shadow_meshlets.get(), shadow_opaque_meshlets.get(),
                shadow_masked_meshlets.get(), shadow_opaque_meshlets.get(),
                shadow_masked_meshlets.get()};
    }
    return {terrain_shadow.get(), shadow_opaque.get(), shadow_masked.get(), shadow_opaque.get(),
            shadow_masked.get()};
}

detail::CategoryPipelines RendererCore::pick_pipelines(bool mesh_shaders) const {
    if (mesh_shaders) {
        return {terrain_id_meshlets.get(), id_meshlets[0].get(), id_meshlets[0].get(),
                id_meshlets[1].get(), id_meshlets[1].get()};
    }
    return {terrain_id.get(), id_pipelines[0].get(), id_pipelines[0].get(), id_pipelines[1].get(),
            id_pipelines[1].get()};
}

} // namespace ngine::render
