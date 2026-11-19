module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

// Skinning auf der GPU (R4.6, ANIM-01, ADR 0050): Netze mit Skin, deren Animator in diesem Frame
// eine Pose hat, verformt ein Compute-Pass vor dem Prüfen der Meshlets in eigene Bereiche des
// Geometriepuffers. Die Instanz zeigt danach auf diese Eckpunkte; alle anderen Pässe bleiben, wie
// sie sind.
module ngine.render;

import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.rhi;
import ngine.asset;
import ngine.world;
import ngine.animation;
import :renderer_detail;

namespace ngine::render {

using rhi::ResourceState;

u64 RendererCore::skinned_vertices(world::World& world, u64 entity, const GpuMesh& mesh,
                                   asset::AssetId model, usize slot) {
    if (animation == nullptr || mesh.skin_address == 0 || !mesh.skin_index) {
        return mesh.vertex_address;
    }
    const std::span<const animation::JointMatrix> palette =
        animation->palette(world, entity, model, *mesh.skin_index);
    if (palette.size() < mesh.joint_count || mesh.joint_count == 0) {
        return mesh.vertex_address;
    }
    SkinnedOutput& output = skinned_outputs[{entity, mesh.vertex_address}];
    output.last_used = frame_number;
    if (output.ranges.empty()) {
        for (usize i = 0; i < frames.size(); ++i) {
            auto range = geometry.allocate(u64{mesh.vertex_count} * sizeof(asset::Vertex));
            if (!range) {
                log::error("Renderer, Skinning: {}", range.error());
                for (const GeometryRange& allocated : output.ranges) {
                    geometry.free(allocated);
                }
                output.ranges.clear();
                return mesh.vertex_address;
            }
            output.ranges.push_back(*range);
        }
    }
    const GeometryRange& target = output.ranges[slot % output.ranges.size()];
    detail::SkinJob job;
    job.push.source = mesh.vertex_address;
    job.push.skin = mesh.skin_address;
    job.push.target = geometry.address(target);
    // Vorläufig der Anfang in palette_data; finish_skinning() setzt die Adresse.
    job.push.palette = palette_data.size();
    job.push.vertex_count = mesh.vertex_count;
    job.push.joint_count = mesh.joint_count;
    job.target = &geometry.buffer(target);
    palette_data.insert(palette_data.end(), palette.begin(), palette.begin() + mesh.joint_count);
    skin_jobs.push_back(job);
    ++stats.skinned;
    stats.skinned_vertices += mesh.vertex_count;
    return job.push.target;
}

void RendererCore::finish_skinning(usize slot) {
    NGINE_PROFILE_ZONE("Renderer::finish_skinning");
    // Bereiche, die zwei Frames lang niemand brauchte, freigeben (der Geometriepuffer nimmt sie
    // erst wieder, wenn kein Frame im Flug sie mehr liest).
    std::erase_if(skinned_outputs, [&](auto& entry) {
        if (entry.second.last_used + 2 >= frame_number) {
            return false;
        }
        for (const GeometryRange& range : entry.second.ranges) {
            geometry.free(range);
        }
        return true;
    });
    if (skin_jobs.empty()) {
        return;
    }
    std::unique_ptr<rhi::Buffer>& buffer = palette_buffers[slot];
    const u64 size = palette_data.size() * sizeof(animation::JointMatrix);
    if (!buffer || buffer->size() < size) {
        auto created = device->create_buffer({.size = std::max<u64>(size * 2, 64 * 1024),
                                              .usage = rhi::BufferUsage::Storage,
                                              .memory = rhi::MemoryUsage::CpuWrite});
        if (!created) {
            log::error("Renderer, Puffer der Paletten: {}", created.error());
            buffer.reset();
            // Ohne Paletten nichts verformen; die Instanzen zeigen dann auf nie geschriebene
            // Bereiche, deshalb lieber gar nicht zeichnen als Unsinn.
            skin_jobs.clear();
            return;
        }
        buffer = *std::move(created);
    }
    buffer->write(0, std::as_bytes(std::span{palette_data}));
    const u64 base = buffer->device_address();
    for (detail::SkinJob& job : skin_jobs) {
        job.push.palette = base + job.push.palette * sizeof(animation::JointMatrix);
    }
}

void RendererCore::add_skinning_pass(RenderGraph& graph) {
    if (skin_jobs.empty()) {
        return;
    }
    graph.add_pass(
        "Skinning", [](PassBuilder& pass) { pass.side_effect(); },
        [this](PassContext& context) {
            context.commands.bind_pipeline(*skinning);
            std::vector<const rhi::Buffer*> targets;
            for (const detail::SkinJob& job : skin_jobs) {
                context.commands.push_constants(std::as_bytes(std::span{&job.push, 1}));
                context.commands.dispatch((job.push.vertex_count + 63) / 64);
                if (std::ranges::find(targets, job.target) == targets.end()) {
                    targets.push_back(job.target);
                }
            }
            // Culling, Schatten, Vorwärts-Pass und ID-Puffer lesen die Eckpunkte danach.
            std::vector<rhi::BufferBarrier> barriers;
            for (const rhi::Buffer* target : targets) {
                barriers.push_back({.buffer = target,
                                    .before = ResourceState::ShaderWrite,
                                    .after = ResourceState::ShaderRead});
            }
            context.commands.barrier({}, barriers);
        });
}

} // namespace ngine::render
