module;

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.rhi;

namespace ngine::render {

namespace {

bool same_shape(const rhi::TextureDesc& a, const rhi::TextureDesc& b) {
    return a.extent.width == b.extent.width && a.extent.height == b.extent.height &&
           a.format == b.format && a.mip_levels == b.mip_levels;
}

bool covers(rhi::TextureUsage have, rhi::TextureUsage want) {
    return (static_cast<u32>(have) & static_cast<u32>(want)) == static_cast<u32>(want);
}

} // namespace

rhi::Texture& TexturePool::acquire(const rhi::TextureDesc& desc, rhi::ResourceState* previous) {
    Entry* found = nullptr;
    for (Entry& entry : entries_) {
        if (entry.in_use || !same_shape(entry.desc, desc) ||
            !covers(entry.desc.usage, desc.usage)) {
            continue;
        }
        if (found == nullptr || entry.desc.usage == desc.usage) {
            found = &entry;
        }
        if (entry.desc.usage == desc.usage) {
            break;
        }
    }
    if (found == nullptr) {
        auto texture = device_->create_texture(desc);
        verify(texture.has_value(), "Transiente Textur lässt sich nicht erzeugen");
        entries_.push_back({.texture = *std::move(texture), .desc = desc});
        found = &entries_.back();
    }
    found->in_use = true;
    found->idle_frames = 0;
    if (previous != nullptr) {
        *previous = found->used_in_frame ? found->state : rhi::ResourceState::Undefined;
    }
    return *found->texture;
}

void TexturePool::release(const rhi::Texture& texture, rhi::ResourceState state) {
    for (Entry& entry : entries_) {
        if (entry.texture.get() == &texture) {
            entry.in_use = false;
            entry.used_in_frame = true;
            entry.state = state;
            return;
        }
    }
}

void TexturePool::end_frame() {
    for (Entry& entry : entries_) {
        entry.idle_frames = entry.in_use || entry.used_in_frame ? 0 : entry.idle_frames + 1;
        entry.in_use = false;
        entry.used_in_frame = false;
        entry.state = rhi::ResourceState::Undefined;
    }
    std::erase_if(entries_, [](const Entry& entry) { return entry.idle_frames > max_idle_frames; });
}

} // namespace ngine::render
