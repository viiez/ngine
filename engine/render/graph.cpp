module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.profile;
import ngine.rhi;

namespace ngine::render {

namespace {

using rhi::ResourceState;

std::string_view state_name(ResourceState state) {
    switch (state) {
    case ResourceState::Undefined: return "Undefined";
    case ResourceState::ColorTarget: return "ColorTarget";
    case ResourceState::DepthTarget: return "DepthTarget";
    case ResourceState::DepthRead: return "DepthRead";
    case ResourceState::ShaderRead: return "ShaderRead";
    case ResourceState::ShaderWrite: return "ShaderWrite";
    case ResourceState::IndexRead: return "IndexRead";
    case ResourceState::TransferSrc: return "TransferSrc";
    case ResourceState::TransferDst: return "TransferDst";
    case ResourceState::Present: return "Present";
    case ResourceState::HostRead: return "HostRead";
    case ResourceState::IndirectRead: return "IndirectRead";
    }
    return "?";
}

rhi::TextureUsage usage_for(ResourceState state) {
    switch (state) {
    case ResourceState::ShaderRead: return rhi::TextureUsage::Sampled;
    case ResourceState::ShaderWrite: return rhi::TextureUsage::Storage;
    case ResourceState::TransferSrc: return rhi::TextureUsage::TransferSrc;
    case ResourceState::TransferDst: return rhi::TextureUsage::TransferDst;
    default: return rhi::TextureUsage::RenderTarget;
    }
}

usize slot(ResourceRef resource, usize texture_count) {
    return resource.kind == ResourceKind::Texture ? resource.index : texture_count + resource.index;
}

} // namespace

// --- PassBuilder ----------------------------------------------------------------------------

void PassBuilder::read(TextureHandle texture, ResourceState state) {
    use({ResourceKind::Texture, texture.index}, state, false);
}
void PassBuilder::write(TextureHandle texture, ResourceState state) {
    use({ResourceKind::Texture, texture.index}, state, true);
}
void PassBuilder::read(BufferHandle buffer, ResourceState state) {
    use({ResourceKind::Buffer, buffer.index}, state, false);
}
void PassBuilder::write(BufferHandle buffer, ResourceState state) {
    use({ResourceKind::Buffer, buffer.index}, state, true);
}
void PassBuilder::side_effect() {
    graph_->passes_[pass_].side_effect = true;
}

void PassBuilder::use(ResourceRef resource, ResourceState state, bool write) {
    RenderGraph::Pass& pass = graph_->passes_[pass_];
    const usize count =
        resource.kind == ResourceKind::Texture ? graph_->textures_.size() : graph_->buffers_.size();
    if (resource.index >= count) {
        graph_->errors_.push_back(std::format("Pass {}: ungültige Ressource", pass.name));
        return;
    }
    const std::string name = graph_->resource_name(resource);
    if (state == ResourceState::Undefined || state == ResourceState::Present) {
        graph_->errors_.push_back(std::format("Pass {}: {} im Zustand {} ist nicht erlaubt",
                                              pass.name, name, state_name(state)));
        return;
    }
    if (rhi::is_write(state) != write) {
        graph_->errors_.push_back(std::format("Pass {}: {} im Zustand {} ist {}", pass.name, name,
                                              state_name(state),
                                              write ? "kein Schreiben" : "kein Lesen"));
        return;
    }
    for (const RenderGraph::Access& access : pass.accesses) {
        if (access.resource == resource) {
            if (access.state != state) {
                graph_->errors_.push_back(std::format("Pass {}: {} in zwei Zuständen ({} und {})",
                                                      pass.name, name, state_name(access.state),
                                                      state_name(state)));
            }
            return;
        }
    }
    pass.accesses.push_back({resource, state, write});
}

// --- PassContext ----------------------------------------------------------------------------

rhi::Texture& PassContext::texture(TextureHandle handle) const {
    rhi::Texture* texture = graph_->textures_[handle.index].texture;
    verify(texture != nullptr, "Textur ohne GPU-Ressource (nicht von diesem Pass benutzt?)");
    return *texture;
}

rhi::Buffer& PassContext::buffer(BufferHandle handle) const {
    return *graph_->buffers_[handle.index].buffer;
}

// --- RenderGraph ----------------------------------------------------------------------------

TextureHandle RenderGraph::import_texture(std::string_view name, rhi::Texture& texture,
                                          ResourceState initial, ResourceState final) {
    compiled_ = false;
    textures_.push_back({.name = std::string{name},
                         .texture = &texture,
                         .imported = true,
                         .desc = {texture.extent(), texture.format()},
                         .initial = initial,
                         .final = final});
    return {static_cast<u32>(textures_.size() - 1)};
}

TextureHandle RenderGraph::create_texture(std::string_view name, const TransientTextureDesc& desc) {
    compiled_ = false;
    textures_.push_back({.name = std::string{name}, .desc = desc});
    return {static_cast<u32>(textures_.size() - 1)};
}

BufferHandle RenderGraph::import_buffer(std::string_view name, rhi::Buffer& buffer,
                                        ResourceState initial, ResourceState final) {
    compiled_ = false;
    buffers_.push_back(
        {.name = std::string{name}, .buffer = &buffer, .initial = initial, .final = final});
    return {static_cast<u32>(buffers_.size() - 1)};
}

void RenderGraph::add_pass(std::string_view name, const Setup& setup, Execute execute) {
    compiled_ = false;
    passes_.push_back({.name = std::string{name}, .execute = std::move(execute)});
    PassBuilder builder{*this, static_cast<u32>(passes_.size() - 1)};
    setup(builder);
}

std::string RenderGraph::resource_name(ResourceRef resource) const {
    return resource.kind == ResourceKind::Texture ? textures_[resource.index].name
                                                  : buffers_[resource.index].name;
}

bool RenderGraph::imported(ResourceRef resource) const {
    return resource.kind == ResourceKind::Buffer || textures_[resource.index].imported;
}

ResourceState RenderGraph::initial_state(ResourceRef resource) const {
    return resource.kind == ResourceKind::Texture ? textures_[resource.index].initial
                                                  : buffers_[resource.index].initial;
}

ResourceState RenderGraph::final_state(ResourceRef resource) const {
    return resource.kind == ResourceKind::Texture ? textures_[resource.index].final
                                                  : buffers_[resource.index].final;
}

rhi::Result<void> RenderGraph::compile() {
    if (!errors_.empty()) {
        std::string message = "Render-Graph:";
        for (const std::string& error : errors_) {
            message += "\n  " + error;
        }
        return std::unexpected(std::move(message));
    }

    plan_ = Plan{};
    const usize texture_count = textures_.size();
    const usize resource_count = texture_count + buffers_.size();

    // 1. Verwerfen, von hinten nach vorn: Ein Pass bleibt, wenn er nach außen wirkt, eine
    //    importierte Ressource schreibt oder etwas schreibt, das ein späterer Pass braucht.
    //    Schreiben zählt auch als Brauchen, weil ein Pass den alten Inhalt laden kann.
    std::vector<bool> live(resource_count, false);
    std::vector<bool> kept(passes_.size(), false);
    for (usize i = passes_.size(); i-- > 0;) {
        const Pass& pass = passes_[i];
        bool keep = pass.side_effect;
        for (const Access& access : pass.accesses) {
            if (access.write &&
                (imported(access.resource) || live[slot(access.resource, texture_count)])) {
                keep = true;
            }
        }
        if (!keep) {
            continue;
        }
        kept[i] = true;
        for (const Access& access : pass.accesses) {
            live[slot(access.resource, texture_count)] = true;
        }
    }

    // 2. Barrieren und Lebensdauer, von vorn nach hinten.
    std::vector<ResourceState> state(resource_count, ResourceState::Undefined);
    std::vector<bool> written(resource_count, false);
    for (u32 t = 0; t < texture_count; ++t) {
        state[t] = textures_[t].initial;
    }
    for (u32 b = 0; b < buffers_.size(); ++b) {
        state[texture_count + b] = buffers_[b].initial;
    }
    plan_.lifetimes.resize(texture_count);
    plan_.usages.resize(texture_count, rhi::TextureUsage{});

    std::vector<std::string> errors;
    for (u32 i = 0; i < passes_.size(); ++i) {
        if (!kept[i]) {
            plan_.culled.push_back(i);
            continue;
        }
        PlannedPass planned{.pass = i};
        for (const Access& access : passes_[i].accesses) {
            const usize s = slot(access.resource, texture_count);
            if (!access.write && !imported(access.resource) && !written[s]) {
                errors.push_back(std::format("Pass {} liest {}, bevor ein Pass sie schreibt",
                                             passes_[i].name, resource_name(access.resource)));
            }
            // Gleicher lesender Zustand braucht keine Barriere; nach einem Schreiben schon,
            // auch im selben Zustand.
            if (state[s] != access.state || rhi::is_write(state[s])) {
                planned.barriers.push_back({access.resource, state[s], access.state});
            }
            state[s] = access.state;
            written[s] = written[s] || access.write;

            if (access.resource.kind == ResourceKind::Texture) {
                Lifetime& lifetime = plan_.lifetimes[access.resource.index];
                if (!lifetime.used()) {
                    lifetime.first = i;
                }
                lifetime.last = i;
                rhi::TextureUsage& usage = plan_.usages[access.resource.index];
                usage = usage | usage_for(access.state);
            }
        }
        plan_.passes.push_back(std::move(planned));
    }
    if (!errors.empty()) {
        errors_ = std::move(errors);
        return compile();
    }

    // 3. Endzustände importierter Ressourcen.
    for (usize s = 0; s < resource_count; ++s) {
        const ResourceRef resource =
            s < texture_count
                ? ResourceRef{ResourceKind::Texture, static_cast<u32>(s)}
                : ResourceRef{ResourceKind::Buffer, static_cast<u32>(s - texture_count)};
        if (!imported(resource)) {
            continue;
        }
        const ResourceState final = final_state(resource);
        if (final != ResourceState::Undefined && state[s] != final) {
            plan_.final_barriers.push_back({resource, state[s], final});
        }
    }
    compiled_ = true;
    return {};
}

void RenderGraph::record_barriers(rhi::CommandList& commands,
                                  const std::vector<PlannedBarrier>& barriers) {
    std::vector<rhi::TextureBarrier> texture_barriers;
    std::vector<rhi::BufferBarrier> buffer_barriers;
    for (const PlannedBarrier& barrier : barriers) {
        if (barrier.resource.kind == ResourceKind::Texture) {
            texture_barriers.push_back(
                {textures_[barrier.resource.index].texture, barrier.before, barrier.after});
        } else {
            buffer_barriers.push_back(
                {buffers_[barrier.resource.index].buffer, barrier.before, barrier.after});
        }
    }
    commands.barrier(texture_barriers, buffer_barriers);
}

void RenderGraph::execute(rhi::CommandList& commands, TexturePool& pool, GpuProfiler* profiler) {
    NGINE_PROFILE_ZONE("RenderGraph::execute");
    if (!compiled_) {
        const auto result = compile();
        verify(result.has_value(), result ? std::string{} : result.error());
    }

    // Transiente Texturen kommen erst vor ihrem ersten Pass aus dem Vorrat und gehen nach ihrem
    // letzten zurück; so teilen sich Texturen mit getrennter Lebensdauer eine (TD-16).
    std::vector<std::vector<u32>> first_use(passes_.size());
    std::vector<std::vector<u32>> last_use(passes_.size());
    for (u32 t = 0; t < textures_.size(); ++t) {
        if (textures_[t].imported || !plan_.lifetimes[t].used()) {
            continue;
        }
        first_use[plan_.lifetimes[t].first].push_back(t);
        last_use[plan_.lifetimes[t].last].push_back(t);
    }
    std::vector<ResourceState> state(textures_.size(), ResourceState::Undefined);

    PassContext context{commands, *this};
    for (const PlannedPass& planned : plan_.passes) {
        if (profiler != nullptr) {
            profiler->begin_zone(commands, passes_[planned.pass].name);
        }
        std::vector<PlannedBarrier> barriers = planned.barriers;
        for (const u32 t : first_use[planned.pass]) {
            TextureEntry& entry = textures_[t];
            ResourceState previous = ResourceState::Undefined;
            entry.texture = &pool.acquire({.extent = entry.desc.extent,
                                           .format = entry.desc.format,
                                           .usage = plan_.usages[t]},
                                          &previous);
            // Benutzte ein früherer Pass die Textur, wartet die erste Barriere auf ihn.
            for (PlannedBarrier& barrier : barriers) {
                if (barrier.resource == ResourceRef{ResourceKind::Texture, t} &&
                    barrier.before == ResourceState::Undefined) {
                    barrier.before = previous;
                }
            }
        }
        for (const PlannedBarrier& barrier : barriers) {
            if (barrier.resource.kind == ResourceKind::Texture) {
                state[barrier.resource.index] = barrier.after;
            }
        }
        record_barriers(commands, barriers);
        if (const Execute& execute = passes_[planned.pass].execute) {
            execute(context);
        }
        for (const u32 t : last_use[planned.pass]) {
            pool.release(*textures_[t].texture, state[t]);
        }
        if (profiler != nullptr) {
            profiler->end_zone(commands);
        }
    }
    record_barriers(commands, plan_.final_barriers);
}

void RenderGraph::reset() {
    textures_.clear();
    buffers_.clear();
    passes_.clear();
    errors_.clear();
    plan_ = Plan{};
    compiled_ = false;
}

} // namespace ngine::render
