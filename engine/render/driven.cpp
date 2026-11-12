module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// GPU-getriebenes Zeichnen (R3.5, ADR 0043): Gruppen von Meshlets aus den Instanzen eines Frames,
// Puffer der Plätze, Prüfen im Rückfallpfad, Hi-Z und die Draws beider Pfade.
module ngine.render;

import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.rhi;
import :renderer_detail;

namespace ngine::render {

using detail::Category;
using detail::DrivenBuffers;
using detail::DrivenPush;
using detail::FrameInfo;
using detail::Job;
using rhi::ResourceState;

namespace {

// Größte Zahl von Arbeitsgruppen in einem Aufruf (Vulkan garantiert 65535 je Achse).
constexpr u32 max_groups_per_call = 65535;

// Block der Statistik einer Aufgabe.
u32 stat_block(Job job) {
    switch (job) {
    case Job::Forward:
    case Job::ForwardLate: return 0;
    case Job::Pick: return 2;
    default: return 1;
    }
}

// Legt einen Puffer an oder vergrößert ihn (mit Luft), wenn er kleiner als size ist.
bool ensure_buffer(rhi::Device& device, std::unique_ptr<rhi::Buffer>& buffer, u64 size,
                   rhi::BufferUsage usage, rhi::MemoryUsage memory, std::string_view name) {
    if (buffer && buffer->size() >= size) {
        return true;
    }
    // Der alte Puffer wird erst freigegeben, wenn die GPU fertig ist (Löschliste).
    auto created =
        device.create_buffer({.size = size + size / 2, .usage = usage, .memory = memory});
    if (!created) {
        log::error("Renderer, {}: {}", name, created.error());
        buffer.reset();
        return false;
    }
    buffer = *std::move(created);
    return true;
}

} // namespace

void RendererCore::build_groups() {
    NGINE_PROFILE_ZONE("Renderer::build_groups");
    groups.clear();
    regions.clear();
    category_groups = {};
    commands_per_job = 0;

    std::vector<u32> order;
    order.reserve(instances.size());
    for (u32 i = 0; i < instances.size(); ++i) {
        if (!instance_keys[i].blend && instances[i].meshlet_count > 0) {
            order.push_back(i);
        }
    }
    std::ranges::stable_sort(order, [&](u32 a, u32 b) {
        const InstanceKey& ka = instance_keys[a];
        const InstanceKey& kb = instance_keys[b];
        return ka.category != kb.category ? ka.category < kb.category : ka.block < kb.block;
    });
    for (const u32 i : order) {
        const InstanceKey& key = instance_keys[i];
        if (regions.empty() || regions.back().category != key.category ||
            regions.back().block != key.block) {
            regions.push_back({.category = key.category,
                               .block = key.block,
                               .index_buffer = key.index_buffer,
                               .command_start = commands_per_job});
        }
        detail::Region& region = regions.back();
        const auto region_index = static_cast<u32>(regions.size() - 1);
        detail::GroupRange& range = category_groups[static_cast<usize>(key.category)];
        if (range.count == 0) {
            range.first = static_cast<u32>(groups.size());
        }
        const u32 count = instances[i].meshlet_count;
        for (u32 first = 0; first < count; first += group_meshlets) {
            groups.push_back({i, first, region.command_start, region_index});
            ++range.count;
        }
        region.max_commands += count;
        commands_per_job += count;
    }
    stats.meshlets = commands_per_job;

    // Durchsichtiges von hinten nach vorn (TD-20).
    std::ranges::stable_sort(blend_draws,
                             [](const detail::BlendDraw& a, const detail::BlendDraw& b) {
                                 return a.distance > b.distance;
                             });
}

bool RendererCore::write_driven(usize slot, std::span<const GpuView, detail::job_count> views,
                                bool mesh_shaders) {
    DrivenBuffers& b = driven[slot];
    const auto storage = rhi::BufferUsage::Storage;
    const auto indirect = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect;
    const u64 instance_bytes = std::max<usize>(instances.size(), 1) * sizeof(GpuInstance);
    const u64 group_bytes = std::max<usize>(groups.size(), 1) * sizeof(groups[0]);
    const u64 counter_bytes =
        detail::stat_bytes + u64{detail::job_count} * std::max<usize>(regions.size(), 1) * 4;
    if (!ensure_buffer(*device, b.instances, instance_bytes, storage, rhi::MemoryUsage::CpuWrite,
                       "Instanzen") ||
        !ensure_buffer(*device, b.groups, group_bytes, storage, rhi::MemoryUsage::CpuWrite,
                       "Gruppen") ||
        !ensure_buffer(*device, b.views, sizeof(GpuView) * detail::job_count, storage,
                       rhi::MemoryUsage::CpuWrite, "Sichten") ||
        !ensure_buffer(*device, b.counters, counter_bytes, indirect, rhi::MemoryUsage::Gpu,
                       "Zähler") ||
        !ensure_buffer(*device, b.readback, detail::stat_bytes, storage, rhi::MemoryUsage::CpuRead,
                       "Statistik")) {
        return false;
    }
    if (!mesh_shaders) {
        const u64 command_bytes = u64{detail::job_count} * std::max<u32>(commands_per_job, 1) *
                                  sizeof(rhi::DrawIndexedCommand);
        if (!ensure_buffer(*device, b.commands, command_bytes, indirect, rhi::MemoryUsage::Gpu,
                           "Befehle")) {
            return false;
        }
    }
    if (!instances.empty()) {
        b.instances->write(0, std::as_bytes(std::span{instances}));
    }
    if (!groups.empty()) {
        b.groups->write(0, std::as_bytes(std::span{groups}));
    }
    b.views->write(0, std::as_bytes(views));
    return true;
}

void RendererCore::read_driven_stats(usize slot) {
    DrivenBuffers& b = driven[slot];
    if (b.readback_pending && b.readback) {
        b.readback->read(0, std::as_writable_bytes(std::span{gpu_stats}));
        b.readback_pending = false;
    }
}

bool RendererCore::ensure_hiz(rhi::Extent2D extent) {
    const rhi::Extent2D size = hiz_extent(extent);
    if (size.width == hiz_size.width && size.height == hiz_size.height &&
        !hiz_sets[0].levels.empty()) {
        return true;
    }
    const u32 levels = hiz_level_count(size);
    for (detail::HizSet& set : hiz_sets) {
        // Alte Texturen gibt das Device frei, wenn die GPU fertig ist.
        set = {};
        for (u32 level = 0; level < levels; ++level) {
            auto texture = device->create_texture(
                {.extent = {std::max(size.width >> level, 1u), std::max(size.height >> level, 1u)},
                 .format = rhi::Format::R32Float,
                 .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage});
            if (!texture) {
                log::error("Renderer, Hi-Z: {}", texture.error());
                hiz_sets = {};
                hiz_size = {};
                return false;
            }
            set.levels.push_back(*std::move(texture));
        }
    }
    hiz_size = size;
    previous_valid = false;
    return true;
}

DrivenPush RendererCore::driven_push(const FrameInfo& frame, Job job, u32 cascade) const {
    const DrivenBuffers& b = *frame.driven;
    const auto j = static_cast<u32>(job);
    const u64 counters = b.counters->device_address();
    return DrivenPush{
        .frame = frame.frame_address,
        .instances = b.instances->device_address(),
        .groups = b.groups->device_address(),
        .view = b.views->device_address() + u64{j} * sizeof(GpuView),
        .materials = materials.address(),
        .terrain_materials = frame.terrain_materials,
        .commands = b.commands ? b.commands->device_address() : 0,
        .counters = counters + detail::stat_bytes,
        .stats = counters + u64{stat_block(job)} * detail::stat_words * sizeof(u32),
        .command_base = j * commands_per_job,
        .counter_base = j * static_cast<u32>(regions.size()),
        .cascade = cascade,
    };
}

void RendererCore::add_clear_pass(RenderGraph& graph, const FrameInfo& frame,
                                  BufferHandle counters) {
    static_cast<void>(frame);
    graph.add_pass(
        "Zähler leeren",
        [&](PassBuilder& pass) { pass.write(counters, ResourceState::TransferDst); },
        [counters](PassContext& context) {
            rhi::Buffer& buffer = context.buffer(counters);
            context.commands.fill_buffer(buffer, 0, buffer.size() & ~u64{3}, 0);
        });
}

void RendererCore::add_cull_pass(RenderGraph& graph, std::string_view name, const FrameInfo& frame,
                                 std::span<const Job> jobs, BufferHandle commands,
                                 BufferHandle counters, std::span<const TextureHandle> hiz_reads) {
    graph.add_pass(
        name,
        [&](PassBuilder& pass) {
            pass.write(commands, ResourceState::ShaderWrite);
            pass.write(counters, ResourceState::ShaderWrite);
            for (const TextureHandle level : hiz_reads) {
                pass.read(level, ResourceState::ShaderRead);
            }
        },
        [this, frame, job_list = std::vector<Job>(jobs.begin(), jobs.end())](PassContext& context) {
            const auto total = static_cast<u32>(groups.size());
            if (total == 0) {
                return;
            }
            context.commands.bind_pipeline(*cull);
            for (const Job job : job_list) {
                const u32 cascade = job >= Job::Shadow0 && job <= Job::Shadow3
                                        ? static_cast<u32>(job) - static_cast<u32>(Job::Shadow0)
                                        : 0;
                DrivenPush push = driven_push(frame, job, cascade);
                for (u32 first = 0; first < total; first += max_groups_per_call) {
                    push.first_group = first;
                    context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                    context.commands.dispatch(std::min(total - first, max_groups_per_call));
                }
            }
        });
}

void RendererCore::add_hiz_passes(RenderGraph& graph, const FrameInfo& frame, TextureHandle depth,
                                  std::span<const TextureHandle> levels) {
    for (u32 level = 0; level < levels.size(); ++level) {
        const TextureHandle source = level == 0 ? depth : levels[level - 1];
        const TextureHandle target = levels[level];
        const rhi::Extent2D source_size =
            level == 0 ? frame.extent
                       : rhi::Extent2D{std::max(frame.hiz_extent.width >> (level - 1), 1u),
                                       std::max(frame.hiz_extent.height >> (level - 1), 1u)};
        const rhi::Extent2D target_size{std::max(frame.hiz_extent.width >> level, 1u),
                                        std::max(frame.hiz_extent.height >> level, 1u)};
        graph.add_pass(
            "Hi-Z",
            [&](PassBuilder& pass) {
                pass.read(source, ResourceState::ShaderRead);
                pass.write(target, ResourceState::ShaderWrite);
            },
            [this, source, target, source_size, target_size](PassContext& context) {
                const detail::HizPush push{.source = context.texture(source).sampled_index(),
                                           .target = context.texture(target).storage_index(),
                                           .source_size = {source_size.width, source_size.height},
                                           .target_size = {target_size.width, target_size.height}};
                context.commands.bind_pipeline(*hiz);
                context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                context.commands.dispatch((target_size.width + 7) / 8,
                                          (target_size.height + 7) / 8);
            });
    }
}

void RendererCore::add_stats_pass(RenderGraph& graph, const FrameInfo& frame,
                                  BufferHandle counters) {
    DrivenBuffers& b = *frame.driven;
    const BufferHandle readback = graph.import_buffer(
        "Statistik lesen", *b.readback, ResourceState::Undefined, ResourceState::HostRead);
    graph.add_pass(
        "Statistik lesen",
        [&](PassBuilder& pass) {
            pass.read(counters, ResourceState::TransferSrc);
            pass.write(readback, ResourceState::TransferDst);
            pass.side_effect();
        },
        [counters, readback](PassContext& context) {
            context.commands.copy_buffer(context.buffer(counters), 0, context.buffer(readback), 0,
                                         detail::stat_bytes);
        });
    b.readback_pending = true;
}

void RendererCore::draw_job(rhi::CommandList& commands, const FrameInfo& frame, Job job,
                            const detail::CategoryPipelines& pipelines, u32 cascade,
                            const rhi::Buffer* command_buffer, const rhi::Buffer* counter_buffer) {
    if (groups.empty()) {
        return;
    }
    DrivenPush push = driven_push(frame, job, cascade);
    if (frame.mesh_shaders) {
        for (u32 c = 0; c < detail::category_count; ++c) {
            const detail::GroupRange range = category_groups[c];
            if (range.count == 0) {
                continue;
            }
            commands.bind_pipeline(*pipelines[c]);
            for (u32 offset = 0; offset < range.count; offset += max_groups_per_call) {
                push.first_group = range.first + offset;
                commands.push_constants(std::as_bytes(std::span{&push, 1}));
                commands.draw_mesh_tasks(std::min(range.count - offset, max_groups_per_call));
            }
        }
        return;
    }
    const rhi::Pipeline* bound = nullptr;
    const rhi::Buffer* indices = nullptr;
    for (u32 r = 0; r < regions.size(); ++r) {
        const detail::Region& region = regions[r];
        const rhi::Pipeline* pipeline = pipelines[static_cast<usize>(region.category)];
        if (bound != pipeline) {
            bound = pipeline;
            commands.bind_pipeline(*bound);
            commands.push_constants(std::as_bytes(std::span{&push, 1}));
        }
        if (indices != region.index_buffer) {
            indices = region.index_buffer;
            commands.bind_index_buffer(*indices);
        }
        commands.draw_indexed_indirect_count(
            *command_buffer,
            u64{push.command_base + region.command_start} * sizeof(rhi::DrawIndexedCommand),
            *counter_buffer, detail::stat_bytes + u64{push.counter_base + r} * sizeof(u32),
            region.max_commands);
    }
}

u32 RendererCore::job_draw_calls(bool mesh_shaders) const {
    if (!mesh_shaders) {
        return static_cast<u32>(regions.size());
    }
    return static_cast<u32>(std::ranges::count_if(
        category_groups, [](const detail::GroupRange& range) { return range.count > 0; }));
}

} // namespace ngine::render
