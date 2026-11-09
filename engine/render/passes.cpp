module;

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// Passes des Renderers (TD-36): Schattenkarten der Sonne, Vorwärts-Pass in ein HDR-Ziel, Tone
// Mapping und der ID-Puffer für die Auswahl per Klick.
module ngine.render;

import ngine.core;
import ngine.math;
import ngine.rhi;
import :renderer_detail;

namespace ngine::render {

using detail::depth_format;
using detail::FrameInfo;
using detail::hdr_format;
using detail::id_format;
using rhi::ResourceState;

std::array<TextureHandle, cascade_count> Renderer::Impl::add_shadow_passes(RenderGraph& graph,
                                                                           const FrameInfo& frame,
                                                                           u64 terrain_materials) {
    std::array<TextureHandle, cascade_count> shadow_maps{};
    if (!frame.shadows_on) {
        return shadow_maps;
    }
    const u32 resolution = frame.shadow_resolution;
    const u64 frame_address = frame.frame_address;
    for (u32 i = 0; i < cascade_count; ++i) {
        shadow_maps[i] = graph.create_texture(
            "Schatten", {.extent = {resolution, resolution}, .format = depth_format});
        const TextureHandle map = shadow_maps[i];
        graph.add_pass(
            "Schatten", [&](PassBuilder& pass) { pass.write(map, ResourceState::DepthTarget); },
            [this, map, i, frame_address, terrain_materials](PassContext& context) {
                context.commands.begin_rendering(
                    {.depth = &context.texture(map), .clear_depth = 1.0f});
                const rhi::Pipeline* bound = nullptr;
                const rhi::Buffer* indices = nullptr;
                for (const detail::ShadowDraw& draw : shadow_draws[i]) {
                    const rhi::Pipeline* pipeline =
                        draw.masked ? shadow_masked.get() : shadow_opaque.get();
                    if (bound != pipeline) {
                        bound = pipeline;
                        context.commands.bind_pipeline(*bound);
                    }
                    if (indices != draw.mesh->index_buffer) {
                        indices = draw.mesh->index_buffer;
                        context.commands.bind_index_buffer(*indices);
                    }
                    const DrawPush push{.model = {draw.model[0], draw.model[1], draw.model[2]},
                                        .vertices = draw.mesh->vertex_address,
                                        .frame = frame_address,
                                        .materials = materials.address(),
                                        .material = draw.material,
                                        .cascade = i};
                    context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                    context.commands.draw_indexed(draw.index_count, 1, draw.first_index);
                    ++stats.shadow_draws;
                }
                if (!terrain_shadow_draws[i].empty()) {
                    context.commands.bind_pipeline(*terrain_shadow);
                    std::vector<detail::TerrainDraw> casters;
                    casters.reserve(terrain_shadow_draws[i].size());
                    for (const u32 index : terrain_shadow_draws[i]) {
                        casters.push_back(terrain_draws[index]);
                    }
                    draw_terrain(context.commands, casters, frame_address, terrain_materials, i);
                    stats.shadow_draws += static_cast<u32>(casters.size());
                }
                context.commands.end_rendering();
            });
    }
    return shadow_maps;
}

TextureHandle
Renderer::Impl::add_forward_pass(RenderGraph& graph, const FrameInfo& frame,
                                 const std::array<TextureHandle, cascade_count>& shadow_maps,
                                 u64 terrain_materials) {
    const TextureHandle hdr =
        graph.create_texture("HDR", {.extent = frame.extent, .format = hdr_format});
    const TextureHandle depth =
        graph.create_texture("Tiefe", {.extent = frame.extent, .format = depth_format});
    graph.add_pass(
        "Vorwärts",
        [&](PassBuilder& pass) {
            pass.write(hdr, ResourceState::ColorTarget);
            pass.write(depth, ResourceState::DepthTarget);
            if (frame.shadows_on) {
                for (const TextureHandle map : shadow_maps) {
                    pass.read(map, ResourceState::ShaderRead);
                }
            }
        },
        [this, hdr, depth, frame, shadow_maps, terrain_materials](PassContext& context) {
            if (frame.shadows_on) {
                std::array<u32, cascade_count> indices{};
                for (u32 i = 0; i < cascade_count; ++i) {
                    indices[i] = context.texture(shadow_maps[i]).sampled_index();
                }
                frame.frame_buffer->write(offsetof(GpuFrame, shadow_maps),
                                          std::as_bytes(std::span{indices}));
            }
            const math::Vec3 sky = frame.sky_color;
            context.commands.begin_rendering({.color = &context.texture(hdr),
                                              .clear_color = rhi::Color{sky.x, sky.y, sky.z, 1.0f},
                                              .depth = &context.texture(depth),
                                              // Umgekehrte Tiefe: 0 ist unendlich fern.
                                              .clear_depth = 0.0f});
            // Terrain zuerst: undurchsichtig, und Durchsichtiges muss danach kommen.
            if (!terrain_draws.empty()) {
                context.commands.bind_pipeline(*terrain);
                draw_terrain(context.commands, terrain_draws, frame.frame_address,
                             terrain_materials, 0);
                for (const detail::TerrainDraw& draw : terrain_draws) {
                    ++stats.draws;
                    stats.triangles += draw.index_count / 3;
                }
            }
            const rhi::Pipeline* bound = nullptr;
            const rhi::Buffer* indices = nullptr;
            for (const detail::Draw& draw : draws) {
                if (bound != pbr[draw.pipeline].get()) {
                    bound = pbr[draw.pipeline].get();
                    context.commands.bind_pipeline(*bound);
                }
                if (indices != draw.mesh->index_buffer) {
                    indices = draw.mesh->index_buffer;
                    context.commands.bind_index_buffer(*indices);
                }
                const DrawPush push{.model = {draw.model[0], draw.model[1], draw.model[2]},
                                    .vertices = draw.mesh->vertex_address,
                                    .frame = frame.frame_address,
                                    .materials = materials.address(),
                                    .material = draw.material};
                context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                context.commands.draw_indexed(draw.index_count, 1, draw.first_index);
                ++stats.draws;
                stats.triangles += draw.index_count / 3;
            }
            if (line_count > 0) {
                const detail::DebugPush push{.lines = frame.lines_address,
                                             .frame = frame.frame_address,
                                             .viewport = {static_cast<f32>(frame.extent.width),
                                                          static_cast<f32>(frame.extent.height)},
                                             .width = debug.width};
                context.commands.bind_pipeline(*debug_lines);
                context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                context.commands.draw(line_count * 6);
            }
            context.commands.end_rendering();
        });
    return hdr;
}

void Renderer::Impl::add_tonemap_pass(RenderGraph& graph, const FrameInfo& frame, TextureHandle hdr,
                                      TextureHandle target) {
    const f32 exposure = frame.exposure;
    graph.add_pass(
        "Tone Mapping",
        [&](PassBuilder& pass) {
            pass.read(hdr, ResourceState::ShaderRead);
            pass.write(target, ResourceState::ColorTarget);
        },
        [this, hdr, target, exposure](PassContext& context) {
            const detail::TonemapPush push{.hdr = context.texture(hdr).sampled_index(),
                                           .exposure = exposure,
                                           .encode_srgb = rhi::is_srgb(target_format) ? 0u : 1u};
            context.commands.begin_rendering(context.texture(target), std::nullopt);
            context.commands.bind_pipeline(*tonemap);
            context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
            context.commands.draw(3);
            context.commands.end_rendering();
        });
}

void Renderer::Impl::add_pick_passes(RenderGraph& graph, usize slot, detail::PickRequest request,
                                     const FrameInfo& frame, u64 terrain_materials) {
    const TextureHandle ids =
        graph.create_texture("IDs", {.extent = frame.extent, .format = id_format});
    const TextureHandle depth =
        graph.create_texture("Tiefe der IDs", {.extent = frame.extent, .format = depth_format});
    const u64 frame_address = frame.frame_address;
    graph.add_pass(
        "Auswahl",
        [&](PassBuilder& pass) {
            pass.write(ids, ResourceState::ColorTarget);
            pass.write(depth, ResourceState::DepthTarget);
        },
        [this, ids, depth, frame_address, terrain_materials](PassContext& context) {
            // 0.0f hat dieselben Bits wie die Ganzzahl 0: „nichts getroffen“.
            context.commands.begin_rendering({.color = &context.texture(ids),
                                              .clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                              .depth = &context.texture(depth),
                                              .clear_depth = 0.0f});
            const rhi::Pipeline* bound = nullptr;
            const rhi::Buffer* indices = nullptr;
            for (const detail::Draw& draw : draws) {
                const rhi::Pipeline* pipeline = id_pipelines[draw.pipeline % 2].get();
                if (bound != pipeline) {
                    bound = pipeline;
                    context.commands.bind_pipeline(*bound);
                }
                if (indices != draw.mesh->index_buffer) {
                    indices = draw.mesh->index_buffer;
                    context.commands.bind_index_buffer(*indices);
                }
                const detail::IdPush push{.model = {draw.model[0], draw.model[1], draw.model[2]},
                                          .vertices = draw.mesh->vertex_address,
                                          .frame = frame_address,
                                          .id = draw.id};
                context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                context.commands.draw_indexed(draw.index_count, 1, draw.first_index);
            }
            if (!terrain_draws.empty()) {
                context.commands.bind_pipeline(*terrain_id);
                draw_terrain(context.commands, terrain_draws, frame_address, terrain_materials, 0);
            }
            context.commands.end_rendering();
        });
    const BufferHandle readback = graph.import_buffer(
        "Auswahl lesen", *pick_buffers[slot], ResourceState::Undefined, ResourceState::HostRead);
    graph.add_pass(
        "Auswahl lesen",
        [&](PassBuilder& pass) {
            pass.read(ids, ResourceState::TransferSrc);
            pass.write(readback, ResourceState::TransferDst);
            pass.side_effect();
        },
        [ids, readback, request](PassContext& context) {
            context.commands.copy_texture_to_buffer(context.texture(ids),
                                                    {.x = static_cast<i32>(request.x),
                                                     .y = static_cast<i32>(request.y),
                                                     .width = 1,
                                                     .height = 1},
                                                    context.buffer(readback), 0);
        });
    pick_pending[slot] = request;
}

} // namespace ngine::render
