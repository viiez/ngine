module;

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.asset;

namespace ngine::render {

// --- MaterialStore --------------------------------------------------------------------------

GpuMaterial to_gpu(const Material& material) {
    return GpuMaterial{
        .base_color = material.base_color,
        .emissive_cutoff = math::extend(material.emissive, material.alpha_cutoff),
        .params = {material.metallic, material.roughness, material.normal_scale,
                   material.occlusion_strength},
        .textures = {material.base_color_texture.texture,
                     material.metallic_roughness_texture.texture, material.normal_texture.texture,
                     material.occlusion_texture.texture},
        .samplers = {material.base_color_texture.sampler,
                     material.metallic_roughness_texture.sampler, material.normal_texture.sampler,
                     material.occlusion_texture.sampler},
        .extra = {material.emissive_texture.texture, material.emissive_texture.sampler,
                  static_cast<u32>(material.alpha_mode), material.double_sided ? 1u : 0u},
    };
}

MaterialStore::MaterialStore(rhi::Device& device) : device_{&device} {
    materials_.push_back(Material{});
}

u32 MaterialStore::add(const Material& material) {
    dirty_ = true;
    if (!free_.empty()) {
        const u32 index = free_.back();
        free_.pop_back();
        materials_[index] = material;
        return index;
    }
    materials_.push_back(material);
    return static_cast<u32>(materials_.size() - 1);
}

void MaterialStore::remove(u32 index) {
    if (index == 0 || index >= materials_.size() ||
        std::ranges::find(free_, index) != free_.end()) {
        return;
    }
    // Bis zur Wiederverwendung zeigt der Platz das Standardmaterial; Draws, die noch im Flug sind,
    // lesen den alten Puffer.
    materials_[index] = Material{};
    free_.push_back(index);
    dirty_ = true;
}

void MaterialStore::set(u32 index, const Material& material) {
    verify(index < materials_.size(), "MaterialStore::set: ungültiger Index");
    materials_[index] = material;
    dirty_ = true;
}

rhi::Result<u32> MaterialStore::add_texture(const asset::Image& image, bool srgb, bool mipmaps) {
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != usize{image.width} * image.height * 4) {
        return std::unexpected("Bild ohne gültige RGBA8-Daten");
    }
    const rhi::Extent2D extent{image.width, image.height};
    auto texture = device_->create_texture({
        .extent = extent,
        .format = srgb ? rhi::Format::RGBA8Srgb : rhi::Format::RGBA8Unorm,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
        .mip_levels = mipmaps ? rhi::full_mip_count(extent) : 1,
    });
    if (!texture) {
        return std::unexpected(texture.error());
    }
    device_->upload(**texture, image.pixels);
    const u32 index = (*texture)->sampled_index();
    textures_.push_back(*std::move(texture));
    return index;
}

rhi::Result<u32> MaterialStore::add_texture_async(rhi::Extent2D extent, rhi::Format format,
                                                  u32 levels, std::span<const std::byte> chain,
                                                  rhi::UploadTicket& ticket) {
    if (extent.width == 0 || extent.height == 0 || levels == 0 ||
        chain.size() != rhi::mip_chain_size(extent, levels, format)) {
        return std::unexpected("Mip-Kette passt nicht zur Textur");
    }
    auto texture = device_->create_texture({
        .extent = extent,
        .format = format,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
        .mip_levels = levels,
    });
    if (!texture) {
        return std::unexpected(texture.error());
    }
    ticket = rhi::later(ticket, device_->upload_async(**texture, chain));
    const u32 index = (*texture)->sampled_index();
    textures_.push_back(*std::move(texture));
    return index;
}

void MaterialStore::remove_texture(u32 index) {
    // Das Device gibt die Textur frei, wenn die GPU sie nicht mehr braucht.
    std::erase_if(textures_, [&](const std::unique_ptr<rhi::Texture>& texture) {
        return texture->sampled_index() == index;
    });
}

rhi::Result<u32> MaterialStore::sampler(const rhi::SamplerDesc& desc) {
    // Anisotropie in Achteln reicht als Schlüssel.
    const auto key =
        std::make_tuple(static_cast<u8>(desc.mag_filter), static_cast<u8>(desc.min_filter),
                        static_cast<u8>(desc.mip_filter), static_cast<u8>(desc.address_u),
                        static_cast<u8>(desc.address_v), static_cast<u32>(desc.anisotropy * 8.0f));
    if (const auto found = sampler_indices_.find(key); found != sampler_indices_.end()) {
        return found->second;
    }
    auto sampler = device_->create_sampler(desc);
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    const u32 index = (*sampler)->index();
    samplers_.push_back(*std::move(sampler));
    sampler_indices_.emplace(key, index);
    return index;
}

void MaterialStore::flush() {
    if (!dirty_) {
        return;
    }
    std::vector<GpuMaterial> data;
    data.reserve(materials_.size());
    for (const Material& material : materials_) {
        data.push_back(to_gpu(material));
    }
    const u64 size = data.size() * sizeof(GpuMaterial);
    if (!buffer_ || buffer_->size() < size) {
        // Mit Luft nach oben; der alte Puffer bleibt bis zum Ende seiner Frames im Flug gültig.
        auto created =
            device_->create_buffer({.size = std::max<u64>(size * 2, 64 * sizeof(GpuMaterial)),
                                    .usage = rhi::BufferUsage::Storage,
                                    .memory = rhi::MemoryUsage::Gpu});
        verify(created.has_value(), "Puffer für Materialien");
        buffer_ = *std::move(created);
    }
    device_->upload(*buffer_, 0, std::as_bytes(std::span{data}));
    dirty_ = false;
}

u64 MaterialStore::address() const {
    verify(buffer_ != nullptr, "MaterialStore::address() vor flush()");
    return buffer_->device_address();
}

// --- MeshStore ------------------------------------------------------------------------------

rhi::Result<u32> MeshStore::add(std::span<const SubmeshData> submeshes, rhi::UploadTicket* ticket) {
    GpuMesh mesh;
    std::vector<asset::Vertex> vertices;
    std::vector<u32> indices;
    math::Vec3 low{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
                   std::numeric_limits<f32>::max()};
    math::Vec3 high = -low;
    for (const SubmeshData& submesh : submeshes) {
        const auto base = static_cast<u32>(vertices.size());
        mesh.submeshes.push_back({.first_index = static_cast<u32>(indices.size()),
                                  .index_count = static_cast<u32>(submesh.indices.size()),
                                  .material = submesh.material});
        for (const u32 index : submesh.indices) {
            if (index >= submesh.vertices.size()) {
                return std::unexpected("MeshStore: Index außerhalb der Eckpunkte");
            }
            indices.push_back(base + index);
        }
        for (const asset::Vertex& vertex : submesh.vertices) {
            low = math::min(low, vertex.position);
            high = math::max(high, vertex.position);
        }
        vertices.insert(vertices.end(), submesh.vertices.begin(), submesh.vertices.end());
    }
    if (!vertices.empty()) {
        mesh.bounds_min = low;
        mesh.bounds_max = high;
    }
    if (!indices.empty()) {
        const auto vertex_bytes = std::as_bytes(std::span{vertices});
        const auto index_bytes = std::as_bytes(std::span{indices});
        auto vertex_range = geometry_->allocate(vertex_bytes.size());
        if (!vertex_range) {
            return std::unexpected(vertex_range.error());
        }
        auto index_range = geometry_->allocate(index_bytes.size());
        if (!index_range) {
            geometry_->free(*vertex_range);
            return std::unexpected(index_range.error());
        }
        if (ticket != nullptr) {
            *ticket = rhi::later(*ticket, geometry_->upload_async(*vertex_range, vertex_bytes));
            *ticket = rhi::later(*ticket, geometry_->upload_async(*index_range, index_bytes));
        } else {
            geometry_->upload(*vertex_range, vertex_bytes);
            geometry_->upload(*index_range, index_bytes);
        }
        mesh.vertices = *vertex_range;
        mesh.indices = *index_range;
        mesh.vertex_address = geometry_->address(*vertex_range);
        mesh.index_buffer = &geometry_->buffer(*index_range);
        const auto first = static_cast<u32>(index_range->offset / sizeof(u32));
        for (Submesh& submesh : mesh.submeshes) {
            submesh.first_index += first;
        }
    }
    if (!free_.empty()) {
        const u32 index = free_.back();
        free_.pop_back();
        meshes_[index] = std::move(mesh);
        return index;
    }
    meshes_.push_back(std::move(mesh));
    return static_cast<u32>(meshes_.size() - 1);
}

void MeshStore::remove(u32 index) {
    if (index >= meshes_.size() || std::ranges::find(free_, index) != free_.end()) {
        return;
    }
    // Die Bereiche werden wiederverwendet, wenn die GPU sie nicht mehr braucht.
    geometry_->free(meshes_[index].vertices);
    geometry_->free(meshes_[index].indices);
    meshes_[index] = GpuMesh{};
    free_.push_back(index);
}

} // namespace ngine::render
