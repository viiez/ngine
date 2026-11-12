module;

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// Passes des Renderers (TD-36): Schattenkarten der Sonne, Vorwärts-Pass in ein HDR-Ziel (seit R3.5
// in zwei Phasen mit Hi-Z dazwischen), Tone Mapping und der ID-Puffer für die Auswahl per Klick.
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

namespace {

// Was ein Pass mit Gruppen von Meshlets im Graph benutzt: im Mesh-Shader-Pfad schreibt der
// Task-Shader die Statistik und liest das Hi-Z, im Rückfallpfad liest der Draw Befehle und Zähler.
void use_driven(PassBuilder& pass, const FrameInfo& frame,
                const RendererCore::DrivenHandles& handles,
                std::span<const TextureHandle> hiz_reads) {
    if (!handles.counters.valid()) {
        return;
    }
    if (frame.mesh_shaders) {
        pass.write(handles.counters, ResourceState::ShaderWrite);
        for (const TextureHandle level : hiz_reads) {
            pass.read(level, ResourceState::ShaderRead);
        }
    } else {
        pass.read(handles.commands, ResourceState::IndirectRead);
        pass.read(handles.counters, ResourceState::IndirectRead);
    }
}

const rhi::Buffer* command_buffer(PassContext& context,
                                  const RendererCore::DrivenHandles& handles) {
    return handles.commands.valid() ? &context.buffer(handles.commands) : nullptr;
}

const rhi::Buffer* counter_buffer(PassContext& context,
                                  const RendererCore::DrivenHandles& handles) {
    return handles.counters.valid() ? &context.buffer(handles.counters) : nullptr;
}

} // namespace

std::array<TextureHandle, cascade_count>
RendererCore::add_shadow_passes(RenderGraph& graph, const FrameInfo& frame,
                                const DrivenHandles& handles) {
    std::array<TextureHandle, cascade_count> shadow_maps{};
    if (!frame.shadows_on) {
        return shadow_maps;
    }
    const u32 resolution = frame.shadow_resolution;
    for (u32 i = 0; i < cascade_count; ++i) {
        shadow_maps[i] = graph.create_texture(
            "Schatten", {.extent = {resolution, resolution}, .format = depth_format});
        const TextureHandle map = shadow_maps[i];
        graph.add_pass(
            "Schatten",
            [&](PassBuilder& pass) {
                pass.write(map, ResourceState::DepthTarget);
                use_driven(pass, frame, handles, {});
            },
            [this, map, i, frame, handles](PassContext& context) {
                context.commands.begin_rendering(
                    {.depth = &context.texture(map), .clear_depth = 1.0f});
                const auto job =
                    static_cast<detail::Job>(static_cast<u32>(detail::Job::Shadow0) + i);
                draw_job(context.commands, frame, job, shadow_pipelines(frame.mesh_shaders), i,
                         command_buffer(context, handles), counter_buffer(context, handles));
                context.commands.end_rendering();
            });
        stats.shadow_draws += job_draw_calls(frame.mesh_shaders);
    }
    return shadow_maps;
}

TextureHandle
RendererCore::add_forward_passes(RenderGraph& graph, const FrameInfo& frame,
                                 const std::array<TextureHandle, cascade_count>& shadow_maps,
                                 const DrivenHandles& handles) {
    const TextureHandle hdr =
        graph.create_texture("HDR", {.extent = frame.extent, .format = hdr_format});
    const TextureHandle depth =
        graph.create_texture("Tiefe", {.extent = frame.extent, .format = depth_format});
    const auto targets = [&](PassBuilder& pass) {
        pass.write(hdr, ResourceState::ColorTarget);
        pass.write(depth, ResourceState::DepthTarget);
        if (frame.shadows_on) {
            for (const TextureHandle map : shadow_maps) {
                pass.read(map, ResourceState::ShaderRead);
            }
        }
    };
    // Durchsichtiges (von hinten nach vorn) und Debug-Linien kommen nach allem Undurchsichtigen.
    const auto finish = [this, frame](rhi::CommandList& commands) {
        const detail::DrivenPush push = blend_draws.empty()
                                            ? detail::DrivenPush{}
                                            : driven_push(frame, detail::Job::Forward, 0);
        const rhi::Pipeline* bound = nullptr;
        const rhi::Buffer* indices = nullptr;
        for (const detail::BlendDraw& draw : blend_draws) {
            if (bound != pbr[draw.pipeline].get()) {
                bound = pbr[draw.pipeline].get();
                commands.bind_pipeline(*bound);
                commands.push_constants(std::as_bytes(std::span{&push, 1}));
            }
            if (indices != draw.index_buffer) {
                indices = draw.index_buffer;
                commands.bind_index_buffer(*indices);
            }
            commands.draw_indexed(draw.index_count, 1, draw.first_index, draw.instance);
        }
        if (line_count > 0) {
            const detail::DebugPush lines{.lines = frame.lines_address,
                                          .frame = frame.frame_address,
                                          .viewport = {static_cast<f32>(frame.extent.width),
                                                       static_cast<f32>(frame.extent.height)},
                                          .width = debug.width};
            commands.bind_pipeline(*debug_lines);
            commands.push_constants(std::as_bytes(std::span{&lines, 1}));
            commands.draw(line_count * 6);
        }
    };

    // Phase 1: was im letzten Frame sichtbar war (ohne Hi-Z: alles im Stumpf).
    graph.add_pass(
        "Vorwärts",
        [&](PassBuilder& pass) {
            targets(pass);
            use_driven(pass, frame, handles, handles.hiz_previous);
        },
        [this, hdr, depth, frame, shadow_maps, handles, finish](PassContext& context) {
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
            draw_job(context.commands, frame, detail::Job::Forward,
                     forward_pipelines(frame.mesh_shaders), 0, command_buffer(context, handles),
                     counter_buffer(context, handles));
            if (!frame.occlusion) {
                finish(context.commands);
            }
            context.commands.end_rendering();
        });
    if (!frame.occlusion) {
        return hdr;
    }

    // Hi-Z aus der Tiefe nach Phase 1, dann Phase 2: was jetzt doch sichtbar ist.
    add_hiz_passes(graph, frame, depth, handles.hiz_current);
    std::vector<TextureHandle> hiz_all = handles.hiz_previous;
    hiz_all.insert(hiz_all.end(), handles.hiz_current.begin(), handles.hiz_current.end());
    if (!frame.mesh_shaders) {
        const std::array jobs{detail::Job::ForwardLate};
        add_cull_pass(graph, "Prüfen (Phase 2)", frame, jobs, handles.commands, handles.counters,
                      hiz_all);
    }
    graph.add_pass(
        "Vorwärts (Phase 2)",
        [&](PassBuilder& pass) {
            targets(pass);
            use_driven(pass, frame, handles, hiz_all);
        },
        [this, hdr, depth, frame, handles, finish](PassContext& context) {
            context.commands.begin_rendering({.color = &context.texture(hdr),
                                              .clear_color = std::nullopt,
                                              .depth = &context.texture(depth),
                                              .clear_depth = std::nullopt});
            draw_job(context.commands, frame, detail::Job::ForwardLate,
                     forward_pipelines(frame.mesh_shaders), 0, command_buffer(context, handles),
                     counter_buffer(context, handles));
            finish(context.commands);
            context.commands.end_rendering();
        });
    return hdr;
}

void RendererCore::add_tonemap_pass(RenderGraph& graph, const FrameInfo& frame, TextureHandle hdr,
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

void RendererCore::add_pick_passes(RenderGraph& graph, usize slot, detail::PickRequest request,
                                   const FrameInfo& frame, const DrivenHandles& handles) {
    const TextureHandle ids =
        graph.create_texture("IDs", {.extent = frame.extent, .format = id_format});
    const TextureHandle depth =
        graph.create_texture("Tiefe der IDs", {.extent = frame.extent, .format = depth_format});
    graph.add_pass(
        "Auswahl",
        [&](PassBuilder& pass) {
            pass.write(ids, ResourceState::ColorTarget);
            pass.write(depth, ResourceState::DepthTarget);
            use_driven(pass, frame, handles, {});
        },
        [this, ids, depth, frame, handles](PassContext& context) {
            // 0.0f hat dieselben Bits wie die Ganzzahl 0: „nichts getroffen“.
            context.commands.begin_rendering({.color = &context.texture(ids),
                                              .clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                              .depth = &context.texture(depth),
                                              .clear_depth = 0.0f});
            draw_job(context.commands, frame, detail::Job::Pick, pick_pipelines(frame.mesh_shaders),
                     0, command_buffer(context, handles), counter_buffer(context, handles));
            // Durchsichtiges zählt bei der Auswahl wie Undurchsichtiges.
            const detail::DrivenPush push = driven_push(frame, detail::Job::Pick, 0);
            const rhi::Pipeline* bound = nullptr;
            const rhi::Buffer* indices = nullptr;
            for (const detail::BlendDraw& draw : blend_draws) {
                const rhi::Pipeline* pipeline = id_pipelines[draw.pipeline % 2].get();
                if (bound != pipeline) {
                    bound = pipeline;
                    context.commands.bind_pipeline(*bound);
                    context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                }
                if (indices != draw.index_buffer) {
                    indices = draw.index_buffer;
                    context.commands.bind_index_buffer(*indices);
                }
                context.commands.draw_indexed(draw.index_count, 1, draw.first_index, draw.instance);
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
