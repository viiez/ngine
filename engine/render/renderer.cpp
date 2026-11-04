module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.reflect;
import ngine.rhi;
import ngine.asset;
import ngine.world;
import ngine.shaders;

namespace ngine::render {

namespace {

using math::Mat4;
using math::Vec3;
using math::Vec4;
using rhi::ResourceState;

constexpr rhi::Format hdr_format = rhi::Format::RGBA16Float;
constexpr rhi::Format depth_format = rhi::Format::D32Float;

struct TonemapPush {
    u32 hdr = 0;
    f32 exposure = 1.0f;
    u32 encode_srgb = 0;
};

bool is_srgb(rhi::Format format) {
    return format == rhi::Format::RGBA8Srgb || format == rhi::Format::BGRA8Srgb;
}

// Drehung aus dem linearen Teil einer Lage, ohne Skalierung (Spalten normiert, rechtwinklig).
math::Mat3 orthonormal(const math::Mat3& linear) {
    const Vec3 z = math::normalize(linear[2]);
    const Vec3 x = math::normalize(math::cross(linear[1], z));
    const Vec3 y = math::cross(z, x);
    return math::Mat3::from_columns(x, y, z);
}

// Pipeline-Variante je Material: durchsichtig oder nicht, doppelseitig oder nicht.
usize variant(const Material& material) {
    return (material.alpha_mode == AlphaMode::Blend ? 2u : 0u) + (material.double_sided ? 1u : 0u);
}

struct Draw {
    std::array<Vec4, 3> model;
    const GpuMesh* mesh = nullptr;
    u32 first_index = 0;
    u32 index_count = 0;
    u32 material = 0;
    usize pipeline = 0;
};

} // namespace

void register_types(reflect::TypeRegistry& types) {
    if (types.find<Camera>() == nullptr) {
        types.add<Camera>("Camera")
            .field("fov_y", &Camera::fov_y)
            .field("near_plane", &Camera::near_plane)
            .field("exposure", &Camera::exposure)
            .field("active", &Camera::active);
    }
    if (types.find<MeshRenderer>() == nullptr) {
        types.add<MeshRenderer>("MeshRenderer")
            .field("mesh", &MeshRenderer::mesh)
            .field("material", &MeshRenderer::material)
            .transient();
    }
}

std::optional<CameraView> find_camera(world::World& world) {
    std::optional<CameraView> found;
    world.each<const world::WorldTransform, const Camera>(
        [&](const world::WorldTransform& transform, const Camera& camera) {
            if (found || !camera.active) {
                return;
            }
            found = CameraView{.position = transform.position(),
                               .rotation = orthonormal(transform.value.linear),
                               .camera = camera};
        });
    return found;
}

Mat4 view_projection(const CameraView& view, f32 aspect) {
    // Kamerarelativ: Die Blickmatrix ist nur die umgekehrte Drehung (ADR 0012).
    const Mat4 rotation = math::to_mat4(math::transpose(view.rotation));
    return math::perspective_reverse_z(view.camera.fov_y, aspect, view.camera.near_plane) *
           rotation;
}

// --- Renderer -------------------------------------------------------------------------------

struct Renderer::Impl {
    rhi::Device* device = nullptr;
    rhi::Format target_format = rhi::Format::Unknown;
    MeshStore meshes;
    MaterialStore materials;
    std::unique_ptr<rhi::Shader> pbr_vertex;
    std::unique_ptr<rhi::Shader> pbr_fragment;
    std::unique_ptr<rhi::Shader> fullscreen_vertex;
    std::unique_ptr<rhi::Shader> tonemap_fragment;
    // Undurchsichtig, undurchsichtig doppelseitig, durchsichtig, durchsichtig doppelseitig.
    std::array<std::unique_ptr<rhi::Pipeline>, 4> pbr;
    std::unique_ptr<rhi::Pipeline> tonemap;
    // Ein Puffer pro Frame im Flug, reihum benutzt.
    std::vector<std::unique_ptr<rhi::Buffer>> frames;
    usize next_frame = 0;
    std::vector<Draw> draws;
    RenderStats stats;

    explicit Impl(rhi::Device& device_) : device{&device_}, meshes{device_}, materials{device_} {}
};

rhi::Result<std::unique_ptr<Renderer>> Renderer::create(rhi::Device& device,
                                                        rhi::Format target_format) {
    auto impl = std::make_unique<Impl>(device);
    impl->target_format = target_format;

    const auto shader = [&](const rhi::ShaderDesc& desc,
                            std::unique_ptr<rhi::Shader>& out) -> rhi::Result<void> {
        auto created = device.create_shader(desc);
        if (!created) {
            return std::unexpected(created.error());
        }
        out = *std::move(created);
        return {};
    };
    for (auto result : {shader(shaders::pbr_vertex(), impl->pbr_vertex),
                        shader(shaders::pbr_fragment(), impl->pbr_fragment),
                        shader(shaders::fullscreen_vertex(), impl->fullscreen_vertex),
                        shader(shaders::tonemap_fragment(), impl->tonemap_fragment)}) {
        if (!result) {
            return std::unexpected("Renderer, Shader: " + result.error());
        }
    }

    for (usize i = 0; i < impl->pbr.size(); ++i) {
        const bool blend = i >= 2;
        auto pipeline = device.create_pipeline({
            .vertex = impl->pbr_vertex.get(),
            .fragment = impl->pbr_fragment.get(),
            .color_format = hdr_format,
            .depth_format = depth_format,
            // Umgekehrte Tiefe: näher heißt größer.
            .depth_compare = rhi::CompareOp::Greater,
            .depth_write = !blend,
            .cull_mode = (i % 2 == 1) ? rhi::CullMode::None : rhi::CullMode::Back,
            .blend = blend ? rhi::BlendMode::Alpha : rhi::BlendMode::None,
        });
        if (!pipeline) {
            return std::unexpected("Renderer, PBR-Pipeline: " + pipeline.error());
        }
        impl->pbr[i] = *std::move(pipeline);
    }
    auto tonemap = device.create_pipeline({.vertex = impl->fullscreen_vertex.get(),
                                           .fragment = impl->tonemap_fragment.get(),
                                           .color_format = target_format});
    if (!tonemap) {
        return std::unexpected("Renderer, Tone Mapping: " + tonemap.error());
    }
    impl->tonemap = *std::move(tonemap);

    for (u32 i = 0; i < std::max(device.frames_in_flight(), 1u); ++i) {
        auto buffer = device.create_buffer({.size = sizeof(GpuFrame),
                                            .usage = rhi::BufferUsage::Storage,
                                            .memory = rhi::MemoryUsage::CpuWrite});
        if (!buffer) {
            return std::unexpected("Renderer, Frame-Puffer: " + buffer.error());
        }
        impl->frames.push_back(*std::move(buffer));
    }
    return std::unique_ptr<Renderer>{new Renderer{std::move(impl)}};
}

Renderer::Renderer(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Renderer::~Renderer() = default;

MeshStore& Renderer::meshes() noexcept {
    return impl_->meshes;
}
MaterialStore& Renderer::materials() noexcept {
    return impl_->materials;
}
const RenderStats& Renderer::stats() const noexcept {
    return impl_->stats;
}

void Renderer::add_passes(RenderGraph& graph, world::World& world, TextureHandle target) {
    NGINE_PROFILE_ZONE("Renderer::add_passes");
    Impl& r = *impl_;
    const rhi::Extent2D extent = graph.texture_desc(target).extent;
    verify(graph.texture_desc(target).format == r.target_format,
           "Renderer::add_passes: Ziel hat nicht das Format aus create()");
    const f32 aspect =
        static_cast<f32>(extent.width) / static_cast<f32>(std::max(extent.height, 1u));

    // Frame-Daten: Kamera und Licht. Der Puffer dieses Frames ist frei, weil reihum so viele
    // Puffer wie Frames im Flug benutzt werden.
    const std::optional<CameraView> camera = find_camera(world);
    rhi::Buffer& frame_buffer = *r.frames[r.next_frame];
    r.next_frame = (r.next_frame + 1) % r.frames.size();
    {
        const Mat4 rows = camera ? math::transpose(view_projection(*camera, aspect)) : Mat4{};
        const Vec3 sun = math::normalize(lighting.sun_direction);
        const GpuFrame frame{
            .view_projection = {rows[0], rows[1], rows[2], rows[3]},
            .sun_direction = math::extend(sun, 0.0f),
            .sun_color = math::extend(lighting.sun_color * lighting.sun_intensity, 0.0f),
            .sky_color = math::extend(lighting.sky_color, 1.0f),
            .ground_color = math::extend(lighting.ground_color, 1.0f),
        };
        frame_buffer.write(0, std::as_bytes(std::span{&frame, 1}));
    }
    r.materials.flush();

    // Draws sammeln, undurchsichtige zuerst.
    r.draws.clear();
    r.stats = {};
    if (camera) {
        world.each<const world::WorldTransform, const MeshRenderer>(
            [&](const world::WorldTransform& transform, const MeshRenderer& renderer) {
                const GpuMesh* mesh = r.meshes.get(renderer.mesh);
                if (mesh == nullptr || !mesh->indices) {
                    return;
                }
                const Mat4 rows = math::transpose(transform.value.relative_to(camera->position));
                for (const Submesh& submesh : mesh->submeshes) {
                    u32 material = renderer.material != rhi::invalid_index ? renderer.material
                                                                           : submesh.material;
                    if (material >= r.materials.size()) {
                        material = 0;
                    }
                    r.draws.push_back({.model = {rows[0], rows[1], rows[2]},
                                       .mesh = mesh,
                                       .first_index = submesh.first_index,
                                       .index_count = submesh.index_count,
                                       .material = material,
                                       .pipeline = variant(r.materials.get(material))});
                }
            });
        std::ranges::stable_partition(r.draws, [](const Draw& draw) { return draw.pipeline < 2; });
    }

    const TextureHandle hdr = graph.create_texture("HDR", {.extent = extent, .format = hdr_format});
    const TextureHandle depth =
        graph.create_texture("Tiefe", {.extent = extent, .format = depth_format});
    const u64 frame_address = frame_buffer.device_address();
    graph.add_pass(
        "Vorwärts",
        [&](PassBuilder& pass) {
            pass.write(hdr, ResourceState::ColorTarget);
            pass.write(depth, ResourceState::DepthTarget);
        },
        [this, hdr, depth, frame_address](PassContext& context) {
            Impl& r = *impl_;
            const Vec3 sky = lighting.sky_color;
            context.commands.begin_rendering({.color = &context.texture(hdr),
                                              .clear_color = rhi::Color{sky.x, sky.y, sky.z, 1.0f},
                                              .depth = &context.texture(depth),
                                              // Umgekehrte Tiefe: 0 ist unendlich fern.
                                              .clear_depth = 0.0f});
            const rhi::Pipeline* bound = nullptr;
            const rhi::Buffer* indices = nullptr;
            for (const Draw& draw : r.draws) {
                if (bound != r.pbr[draw.pipeline].get()) {
                    bound = r.pbr[draw.pipeline].get();
                    context.commands.bind_pipeline(*bound);
                }
                if (indices != draw.mesh->indices.get()) {
                    indices = draw.mesh->indices.get();
                    context.commands.bind_index_buffer(*indices);
                }
                const DrawPush push{.model = {draw.model[0], draw.model[1], draw.model[2]},
                                    .vertices = draw.mesh->vertices->device_address(),
                                    .frame = frame_address,
                                    .materials = r.materials.address(),
                                    .material = draw.material};
                context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                context.commands.draw_indexed(draw.index_count, 1, draw.first_index);
                ++r.stats.draws;
                r.stats.triangles += draw.index_count / 3;
            }
            context.commands.end_rendering();
        });

    const f32 exposure = camera ? camera->camera.exposure : 1.0f;
    graph.add_pass(
        "Tone Mapping",
        [&](PassBuilder& pass) {
            pass.read(hdr, ResourceState::ShaderRead);
            pass.write(target, ResourceState::ColorTarget);
        },
        [this, hdr, target, exposure](PassContext& context) {
            const TonemapPush push{.hdr = context.texture(hdr).sampled_index(),
                                   .exposure = exposure,
                                   .encode_srgb = is_srgb(impl_->target_format) ? 0u : 1u};
            context.commands.begin_rendering(context.texture(target), std::nullopt);
            context.commands.bind_pipeline(*impl_->tonemap);
            context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
            context.commands.draw(3);
            context.commands.end_rendering();
        });
}

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
