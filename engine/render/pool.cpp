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

bool same(const rhi::TextureDesc& a, const rhi::TextureDesc& b) {
    return a.extent.width == b.extent.width && a.extent.height == b.extent.height &&
           a.format == b.format && a.usage == b.usage;
}

} // namespace

rhi::Texture& TexturePool::acquire(const rhi::TextureDesc& desc) {
    for (Entry& entry : entries_) {
        if (!entry.in_use && same(entry.desc, desc)) {
            entry.in_use = true;
            entry.idle_frames = 0;
            return *entry.texture;
        }
    }
    auto texture = device_->create_texture(desc);
    verify(texture.has_value(), "Transiente Textur lässt sich nicht erzeugen");
    entries_.push_back({.texture = *std::move(texture), .desc = desc, .in_use = true});
    return *entries_.back().texture;
}

void TexturePool::end_frame() {
    for (Entry& entry : entries_) {
        entry.idle_frames = entry.in_use ? 0 : entry.idle_frames + 1;
        entry.in_use = false;
    }
    std::erase_if(entries_, [](const Entry& entry) { return entry.idle_frames > max_idle_frames; });
}

} // namespace ngine::render
