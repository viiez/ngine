module;

#include <ngine/profile.hpp>

#include <atomic>
#include <cstddef>
#include <expected>
#include <format>
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
import :renderer_detail;

namespace ngine::render {

// --- Modelle --------------------------------------------------------------------------------

// --- GpuModel -------------------------------------------------------------------------------

GpuModel::GpuModel(Renderer& renderer, ModelInstance instance, asset::Model layout,
                   rhi::UploadTicket ticket)
    : renderer_{&renderer}, instance_{std::move(instance)}, layout_{std::move(layout)},
      ticket_{ticket} {}

GpuModel::~GpuModel() {
    release();
}

GpuModel::GpuModel(GpuModel&& other) noexcept
    : renderer_{std::exchange(other.renderer_, nullptr)}, instance_{std::move(other.instance_)},
      layout_{std::move(other.layout_)}, ticket_{other.ticket_} {}

GpuModel& GpuModel::operator=(GpuModel&& other) noexcept {
    if (this != &other) {
        release();
        renderer_ = std::exchange(other.renderer_, nullptr);
        instance_ = std::move(other.instance_);
        layout_ = std::move(other.layout_);
        ticket_ = other.ticket_;
    }
    return *this;
}

void GpuModel::release() noexcept {
    if (renderer_ == nullptr) {
        return;
    }
    for (const u32 mesh : instance_.meshes) {
        renderer_->meshes().remove(mesh);
    }
    for (const u32 material : instance_.materials) {
        renderer_->materials().remove(material);
    }
    for (const u32 texture : instance_.textures) {
        renderer_->materials().remove_texture(texture);
    }
    renderer_ = nullptr;
}

// --- Modelle --------------------------------------------------------------------------------

namespace {

// Ruft fn(Bild, srgb, mipmaps) für jede Textur, die ein Material benutzt.
template <typename Fn>
void for_each_texture(const asset::Model& model, Fn&& fn) {
    const auto visit = [&](const std::optional<u32>& texture, bool srgb) {
        if (!texture || *texture >= model.textures.size()) {
            return;
        }
        const asset::Texture& source = model.textures[*texture];
        if (source.image < model.images.size()) {
            fn(source.image, srgb, source.sampler.mipmaps);
        }
    };
    for (const asset::Material& material : model.materials) {
        visit(material.base_color_texture, true);
        visit(material.metallic_roughness_texture, false);
        visit(material.normal_texture, false);
        visit(material.occlusion_texture, false);
        visit(material.emissive_texture, true);
    }
}

} // namespace

namespace {

std::atomic<u32> loads_cached{0};
std::atomic<u32> loads_cooked{0};
std::atomic<u32> loads_source{0};

rhi::Format to_rhi(asset::TextureFormat format, bool srgb) {
    if (format == asset::TextureFormat::BC7) {
        return srgb ? rhi::Format::BC7Srgb : rhi::Format::BC7Unorm;
    }
    return srgb ? rhi::Format::RGBA8Srgb : rhi::Format::RGBA8Unorm;
}

} // namespace

ModelTextures prepare_textures(const asset::Model& model) {
    NGINE_PROFILE_ZONE("render::prepare_textures");
    ModelTextures result;
    for_each_texture(model, [&](u32 image, bool srgb, bool mipmaps) {
        const auto key = std::make_pair(image, srgb);
        if (!result.textures.contains(key)) {
            result.textures.emplace(key,
                                    asset::texture_from_image(model.images[image], srgb, mipmaps));
        }
    });
    return result;
}

asset::Result<ModelTextures> decode_textures(asset::CookedModel& cooked,
                                             asset::TextureFormat target) {
    NGINE_PROFILE_ZONE("render::decode_textures");
    ModelTextures result;
    for (asset::CookedTexture& texture : cooked.textures) {
        if (target == asset::TextureFormat::BC7 && !texture.bc7.data.empty()) {
            texture.bc7.srgb = texture.srgb;
            result.textures.insert_or_assign({texture.image, texture.srgb}, std::move(texture.bc7));
            continue;
        }
        auto decoded = asset::decode_ktx2(texture.ktx2, target);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        // Der Farbraum kommt aus dem Material, nicht aus der Datei.
        decoded->srgb = texture.srgb;
        result.textures.insert_or_assign({texture.image, texture.srgb}, *std::move(decoded));
    }
    return result;
}

rhi::Result<GpuModel> upload_model(Renderer& renderer, const asset::Model& model,
                                   const ModelTextures* textures) {
    NGINE_PROFILE_ZONE("render::upload_model");
    MaterialStore& materials = renderer.materials();
    ModelInstance instance;
    rhi::UploadTicket ticket;
    // Gibt bei einem Fehler unterwegs wieder frei, was schon hochgeladen ist.
    GpuModel partial{renderer, {}, {}};

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
            const asset::Image& image = model.images[source.image];
            const asset::TextureData* ready = nullptr;
            if (textures != nullptr) {
                if (const auto it = textures->textures.find(key); it != textures->textures.end()) {
                    ready = &it->second;
                }
            }
            auto index = ready != nullptr
                             ? materials.add_texture_async({ready->width, ready->height},
                                                           to_rhi(ready->format, srgb),
                                                           ready->levels, ready->data, ticket)
                             : materials.add_texture(image, srgb, source.sampler.mipmaps);
            if (!index) {
                error = index.error();
                return {};
            }
            instance.textures.push_back(*index);
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
            partial = GpuModel{renderer, std::move(instance), {}};
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
            submeshes.push_back({.vertices = primitive.vertices,
                                 .indices = primitive.indices,
                                 .material = material,
                                 .meshlets = primitive.meshlets});
        }
        auto index = renderer.meshes().add(submeshes, textures != nullptr ? &ticket : nullptr);
        if (!index) {
            partial = GpuModel{renderer, std::move(instance), {}};
            return std::unexpected("Modell, Mesh " + mesh.name + ": " + index.error());
        }
        instance.meshes.push_back(*index);
    }

    // Aufbau ohne die großen Daten, die jetzt auf der GPU liegen.
    asset::Model layout;
    layout.materials = model.materials;
    layout.textures = model.textures;
    layout.nodes = model.nodes;
    layout.roots = model.roots;
    for (const asset::Mesh& mesh : model.meshes) {
        asset::Mesh& copy = layout.meshes.emplace_back();
        copy.name = mesh.name;
        for (const asset::Primitive& primitive : mesh.primitives) {
            copy.primitives.push_back({.vertices = {},
                                       .indices = {},
                                       .material = primitive.material,
                                       .generated_tangents = primitive.generated_tangents});
        }
    }
    for (const asset::Image& image : model.images) {
        layout.images.push_back(
            {.name = image.name, .width = image.width, .height = image.height, .pixels = {}});
    }
    return GpuModel{renderer, std::move(instance), std::move(layout), ticket};
}

void register_loaders(asset::ResourceManager& resources, Renderer& renderer, ModelLoading loading) {
    // Texturen aus Bilddateien (R3.2), seit R4.3 gekocht aus demselben Cache.
    detail::register_texture_loaders(resources, renderer, loading.cache,
                                     std::move(loading.cook_image));
    // Was im Hintergrund entsteht: das Modell und seine fertigen Texturen.
    struct Prepared {
        asset::Model model;
        ModelTextures textures;
    };
    const asset::TextureFormat target = renderer.device().capabilities().bc_compression
                                            ? asset::TextureFormat::BC7
                                            : asset::TextureFormat::RGBA8;
    const auto from_cooked =
        [target](asset::CookedModel& cooked,
                 const asset::LoadContext& context) -> asset::Result<Prepared> {
        auto textures = decode_textures(cooked, target);
        if (!textures) {
            return std::unexpected(std::format("{}: {}", context.path, textures.error()));
        }
        if (context.dependencies != nullptr) {
            for (const asset::SourceStamp& stamp : cooked.sources) {
                context.dependencies->push_back(stamp.path);
            }
        }
        return Prepared{.model = std::move(cooked.model), .textures = *std::move(textures)};
    };
    resources.add_loader<GpuModel, Prepared>(
        [loading = std::move(loading),
         from_cooked](const asset::LoadContext& context) -> asset::Result<Prepared> {
            // 1. Aktuell gekocht im Cache.
            if (loading.cache != nullptr) {
                auto cached = loading.cache->load_model(context.id);
                if (cached && *cached) {
                    ++loads_cached;
                    return from_cooked(**cached, context);
                }
            }
            // 2. Jetzt kochen und in den Cache legen (Editor).
            if (loading.cook) {
                auto cooked = loading.cook(context.path);
                if (!cooked) {
                    return std::unexpected(std::move(cooked.error()));
                }
                if (loading.cache != nullptr) {
                    if (auto stored = loading.cache->store_model(context.id, *cooked); !stored) {
                        log::warn("Cache: {}", stored.error());
                    }
                }
                ++loads_cooked;
                return from_cooked(*cooked, context);
            }
            // 3. Aus der Quelle.
            auto model = asset::load_gltf(context.path, context.dependencies);
            if (!model) {
                return std::unexpected(std::move(model.error()));
            }
            ++loads_source;
            Prepared prepared{.model = *std::move(model), .textures = {}};
            prepared.textures = prepare_textures(prepared.model);
            // Meshlets im Hintergrund statt beim Hochladen (gekochte Modelle haben sie schon).
            asset::ensure_meshlets(prepared.model);
            return prepared;
        },
        [&renderer](Prepared& prepared) -> asset::Result<GpuModel> {
            auto uploaded = upload_model(renderer, prepared.model, &prepared.textures);
            if (!uploaded) {
                return std::unexpected(std::move(uploaded.error()));
            }
            return std::move(*uploaded);
        },
        // TD-30: erst eintauschen, wenn die Uploads im Hintergrund fertig sind.
        [&renderer](const GpuModel& model) {
            return renderer.device().upload_done(model.ticket());
        });
}

ModelLoadCounts model_load_counts() noexcept {
    return {.cached = loads_cached.load(),
            .cooked = loads_cooked.load(),
            .source = loads_source.load()};
}

world::Entity spawn_model(world::World& world, const asset::Model& model, asset::AssetId model_id,
                          std::string_view name) {
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
        if (node.mesh && *node.mesh < model.meshes.size()) {
            entity.set(MeshRenderer{.mesh = {.asset = model_id, .index = *node.mesh}});
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
