module;

#include <ngine/profile.hpp>

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Modelle aus ngine.asset in den Renderer und in die Welt bringen.
module ngine.render;

import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.rhi;
import ngine.asset;
import ngine.world;

namespace ngine::render {

// --- Modelle --------------------------------------------------------------------------------

rhi::Result<ModelInstance> upload_model(Renderer& renderer, const asset::Model& model) {
    NGINE_PROFILE_ZONE("render::upload_model");
    MaterialStore& materials = renderer.materials();
    ModelInstance instance;

    // Jedes Bild höchstens einmal je Farbraum.
    std::map<std::pair<u32, bool>, u32> uploaded;
    std::optional<std::string> error;
    const auto texture_ref = [&](const std::optional<u32>& texture, bool srgb) -> TextureRef {
        if (!texture || error || *texture >= model.textures.size()) {
            return {};
        }
        const asset::Texture& source = model.textures[*texture];
        if (source.image >= model.images.size()) {
            return {};
        }
        const auto key = std::make_pair(source.image, srgb);
        auto found = uploaded.find(key);
        if (found == uploaded.end()) {
            auto index =
                materials.add_texture(model.images[source.image], srgb, source.sampler.mipmaps);
            if (!index) {
                error = index.error();
                return {};
            }
            found = uploaded.emplace(key, *index).first;
        }
        const auto filter = [](asset::Filter f) {
            return f == asset::Filter::Nearest ? rhi::Filter::Nearest : rhi::Filter::Linear;
        };
        const auto address = [](asset::Wrap w) {
            switch (w) {
            case asset::Wrap::ClampToEdge: return rhi::AddressMode::ClampToEdge;
            case asset::Wrap::MirroredRepeat: return rhi::AddressMode::MirroredRepeat;
            default: return rhi::AddressMode::Repeat;
            }
        };
        auto sampler = materials.sampler({.mag_filter = filter(source.sampler.mag),
                                          .min_filter = filter(source.sampler.min),
                                          .mip_filter = filter(source.sampler.mip),
                                          .address_u = address(source.sampler.wrap_u),
                                          .address_v = address(source.sampler.wrap_v),
                                          .anisotropy = 8.0f});
        if (!sampler) {
            error = sampler.error();
            return {};
        }
        return {found->second, *sampler};
    };

    for (const asset::Material& source : model.materials) {
        Material material{
            .base_color = source.base_color,
            .metallic = source.metallic,
            .roughness = source.roughness,
            .emissive = source.emissive,
            .normal_scale = source.normal_scale,
            .occlusion_strength = source.occlusion_strength,
            .base_color_texture = texture_ref(source.base_color_texture, true),
            .metallic_roughness_texture = texture_ref(source.metallic_roughness_texture, false),
            .normal_texture = texture_ref(source.normal_texture, false),
            .occlusion_texture = texture_ref(source.occlusion_texture, false),
            .emissive_texture = texture_ref(source.emissive_texture, true),
            .alpha_mode = source.alpha_mode == asset::AlphaMode::Blend  ? AlphaMode::Blend
                          : source.alpha_mode == asset::AlphaMode::Mask ? AlphaMode::Mask
                                                                        : AlphaMode::Opaque,
            .alpha_cutoff = source.alpha_cutoff,
            .double_sided = source.double_sided,
        };
        if (error) {
            return std::unexpected("Modell, Textur: " + *error);
        }
        instance.materials.push_back(materials.add(material));
    }

    for (const asset::Mesh& mesh : model.meshes) {
        std::vector<SubmeshData> submeshes;
        for (const asset::Primitive& primitive : mesh.primitives) {
            const u32 material =
                primitive.material && *primitive.material < instance.materials.size()
                    ? instance.materials[*primitive.material]
                    : 0;
            submeshes.push_back({primitive.vertices, primitive.indices, material});
        }
        auto index = renderer.meshes().add(submeshes);
        if (!index) {
            return std::unexpected("Modell, Mesh " + mesh.name + ": " + index.error());
        }
        instance.meshes.push_back(*index);
    }
    return instance;
}

world::Entity spawn_model(world::World& world, const asset::Model& model,
                          const ModelInstance& instance, std::string_view name) {
    world::Entity root = world.create(name);
    root.set(world::Transform{});
    // Rekursiv über die Hierarchie; die Tiefe ist durch die Prüfung auf Zyklen beim Laden
    // begrenzt.
    const std::function<void(u32, world::Entity)> spawn = [&](u32 index, world::Entity parent) {
        const asset::Node& node = model.nodes[index];
        world::Entity entity = world.create(node.name);
        entity.set(world::Transform{.position = math::to_f64(node.translation),
                                    .rotation = node.rotation,
                                    .scale = node.scale});
        if (node.mesh && *node.mesh < instance.meshes.size()) {
            entity.set(MeshRenderer{.mesh = instance.meshes[*node.mesh]});
        }
        entity.set_parent(parent);
        for (const u32 child : node.children) {
            spawn(child, entity);
        }
    };
    for (const u32 node : model.roots) {
        spawn(node, root);
    }
    return root;
}

} // namespace ngine::render
