module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
constexpr rhi::Format id_format = rhi::Format::R32Uint;

struct TonemapPush {
    u32 hdr = 0;
    f32 exposure = 1.0f;
    u32 encode_srgb = 0;
};

bool is_srgb(rhi::Format format) {
    return format == rhi::Format::RGBA8Srgb || format == rhi::Format::BGRA8Srgb;
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
    // Untere 32 Bit der Entity für den ID-Puffer (World::entity_from_index()).
    u32 id = 0;
};

// Push Constants des ID-Puffers, wie id.slang sie liest.
struct IdPush {
    math::Vec4 model[3];
    u64 vertices = 0;
    u64 frame = 0;
    u32 id = 0;
    u32 reserved = 0;
};
static_assert(sizeof(IdPush) <= rhi::max_push_constant_size);

struct PickRequest {
    u32 x = 0;
    u32 y = 0;
};

} // namespace

// --- Renderer -------------------------------------------------------------------------------

namespace {

struct ShadowDraw {
    std::array<Vec4, 3> model;
    const GpuMesh* mesh = nullptr;
    u32 first_index = 0;
    u32 index_count = 0;
    u32 material = 0;
    bool masked = false;
};

struct DebugPush {
    u64 lines = 0;
    u64 frame = 0;
    f32 viewport[2] = {};
    f32 width = 1.0f;
    u32 reserved = 0;
};

// Hüllkugel eines Meshes an seiner Lage, relativ zu origin.
std::pair<Vec3, f32> bounding_sphere(const GpuMesh& mesh, const math::Affine& transform,
                                     math::DVec3 origin) {
    const Vec3 local_center = (mesh.bounds_min + mesh.bounds_max) * 0.5f;
    const f32 local_radius = math::length(mesh.bounds_max - mesh.bounds_min) * 0.5f;
    const f32 scale =
        std::max({math::length(transform.linear[0]), math::length(transform.linear[1]),
                  math::length(transform.linear[2])});
    const Vec3 center =
        math::to_f32(transform.transform_point(math::to_f64(local_center)) - origin);
    return {center, local_radius * scale};
}

} // namespace

struct Renderer::Impl {
    rhi::Device* device = nullptr;
    rhi::Format target_format = rhi::Format::Unknown;
    MeshStore meshes;
    MaterialStore materials;
    DebugDraw debug;
    std::unique_ptr<rhi::Shader> pbr_vertex;
    std::unique_ptr<rhi::Shader> pbr_fragment;
    std::unique_ptr<rhi::Shader> shadow_vertex;
    std::unique_ptr<rhi::Shader> shadow_fragment;
    std::unique_ptr<rhi::Shader> debug_vertex;
    std::unique_ptr<rhi::Shader> debug_fragment;
    std::unique_ptr<rhi::Shader> fullscreen_vertex;
    std::unique_ptr<rhi::Shader> tonemap_fragment;
    std::unique_ptr<rhi::Shader> id_vertex;
    std::unique_ptr<rhi::Shader> id_fragment;
    // ID-Puffer mit und ohne Aussortieren von Rückseiten (wie die PBR-Varianten).
    std::array<std::unique_ptr<rhi::Pipeline>, 2> id_pipelines;
    // Pro Frame im Flug: Puffer für das zurückgelesene Pixel und die Anfrage, die er beantwortet.
    std::vector<std::unique_ptr<rhi::Buffer>> pick_buffers;
    std::vector<std::optional<PickRequest>> pick_pending;
    std::optional<PickRequest> pick_requested;
    std::optional<PickRequest> pick_answered;
    u32 pick_answer = 0;
    // Undurchsichtig, undurchsichtig doppelseitig, durchsichtig, durchsichtig doppelseitig.
    std::array<std::unique_ptr<rhi::Pipeline>, 4> pbr;
    // Schatten ohne und mit Alpha-Maske; neu erzeugt, wenn sich der Tiefenversatz ändert.
    std::unique_ptr<rhi::Pipeline> shadow_opaque;
    std::unique_ptr<rhi::Pipeline> shadow_masked;
    f32 shadow_bias_constant = 0.0f;
    f32 shadow_bias_slope = 0.0f;
    std::unique_ptr<rhi::Pipeline> debug_lines;
    std::unique_ptr<rhi::Pipeline> tonemap;
    std::unique_ptr<rhi::Sampler> shadow_sampler;
    // Je ein Puffer pro Frame im Flug, reihum benutzt: Frame-Daten, Lichter, Debug-Linien.
    std::vector<std::unique_ptr<rhi::Buffer>> frames;
    std::vector<std::unique_ptr<rhi::Buffer>> lights;
    std::vector<std::unique_ptr<rhi::Buffer>> lines;
    usize next_frame = 0;
    std::vector<Draw> draws;
    std::array<std::vector<ShadowDraw>, cascade_count> shadow_draws;
    u32 line_count = 0;
    RenderStats stats;

    explicit Impl(rhi::Device& device_) : device{&device_}, meshes{device_}, materials{device_} {}

    rhi::Result<void> create_shadow_pipelines(f32 constant, f32 slope) {
        for (const bool masked : {false, true}) {
            auto pipeline = device->create_pipeline({
                .vertex = shadow_vertex.get(),
                .fragment = masked ? shadow_fragment.get() : nullptr,
                .depth_format = depth_format,
                .depth_compare = rhi::CompareOp::LessOrEqual,
                .depth_bias_constant = constant,
                .depth_bias_slope = slope,
                // Ohne Aussortieren werfen auch einseitige Flächen von hinten Schatten.
                .cull_mode = rhi::CullMode::None,
            });
            if (!pipeline) {
                return std::unexpected("Renderer, Schatten-Pipeline: " + pipeline.error());
            }
            (masked ? shadow_masked : shadow_opaque) = *std::move(pipeline);
        }
        shadow_bias_constant = constant;
        shadow_bias_slope = slope;
        return {};
    }

    // Puffer für Debug-Linien dieses Frames, mindestens count Linien groß.
    rhi::Result<rhi::Buffer*> line_buffer(usize slot, usize count) {
        std::unique_ptr<rhi::Buffer>& buffer = lines[slot];
        const u64 needed = std::max<u64>(count, 1024) * sizeof(GpuDebugLine);
        if (!buffer || buffer->size() < needed) {
            // Der alte Puffer wird erst freigegeben, wenn die GPU fertig ist (Löschliste).
            auto created = device->create_buffer({.size = needed * 2,
                                                  .usage = rhi::BufferUsage::Storage,
                                                  .memory = rhi::MemoryUsage::CpuWrite});
            if (!created) {
                return std::unexpected(created.error());
            }
            buffer = *std::move(created);
        }
        return buffer.get();
    }

    // ID-Puffer mit allen Draws dieses Frames und Kopie eines Pixels in pick_buffers[slot].
    void add_pick_passes(RenderGraph& graph, usize slot, PickRequest request, rhi::Extent2D extent,
                         u64 frame_address) {
        const TextureHandle ids =
            graph.create_texture("IDs", {.extent = extent, .format = id_format});
        const TextureHandle depth =
            graph.create_texture("Tiefe der IDs", {.extent = extent, .format = depth_format});
        graph.add_pass(
            "Auswahl",
            [&](PassBuilder& pass) {
                pass.write(ids, ResourceState::ColorTarget);
                pass.write(depth, ResourceState::DepthTarget);
            },
            [this, ids, depth, frame_address](PassContext& context) {
                // 0.0f hat dieselben Bits wie die Ganzzahl 0: „nichts getroffen“.
                context.commands.begin_rendering({.color = &context.texture(ids),
                                                  .clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                                  .depth = &context.texture(depth),
                                                  .clear_depth = 0.0f});
                const rhi::Pipeline* bound = nullptr;
                const rhi::Buffer* indices = nullptr;
                for (const Draw& draw : draws) {
                    const rhi::Pipeline* pipeline = id_pipelines[draw.pipeline % 2].get();
                    if (bound != pipeline) {
                        bound = pipeline;
                        context.commands.bind_pipeline(*bound);
                    }
                    if (indices != draw.mesh->indices.get()) {
                        indices = draw.mesh->indices.get();
                        context.commands.bind_index_buffer(*indices);
                    }
                    const IdPush push{.model = {draw.model[0], draw.model[1], draw.model[2]},
                                      .vertices = draw.mesh->vertices->device_address(),
                                      .frame = frame_address,
                                      .id = draw.id};
                    context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                    context.commands.draw_indexed(draw.index_count, 1, draw.first_index);
                }
                context.commands.end_rendering();
            });
        const BufferHandle readback =
            graph.import_buffer("Auswahl lesen", *pick_buffers[slot], ResourceState::Undefined,
                                ResourceState::HostRead);
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
                        shader(shaders::shadow_vertex(), impl->shadow_vertex),
                        shader(shaders::shadow_fragment(), impl->shadow_fragment),
                        shader(shaders::debug_vertex(), impl->debug_vertex),
                        shader(shaders::debug_fragment(), impl->debug_fragment),
                        shader(shaders::fullscreen_vertex(), impl->fullscreen_vertex),
                        shader(shaders::tonemap_fragment(), impl->tonemap_fragment),
                        shader(shaders::id_vertex(), impl->id_vertex),
                        shader(shaders::id_fragment(), impl->id_fragment)}) {
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
    const ShadowSettings defaults;
    if (auto result =
            impl->create_shadow_pipelines(defaults.depth_bias_constant, defaults.depth_bias_slope);
        !result) {
        return std::unexpected(result.error());
    }
    auto debug_lines = device.create_pipeline({.vertex = impl->debug_vertex.get(),
                                               .fragment = impl->debug_fragment.get(),
                                               .color_format = hdr_format,
                                               .depth_format = depth_format,
                                               .depth_compare = rhi::CompareOp::GreaterOrEqual,
                                               .depth_write = false});
    if (!debug_lines) {
        return std::unexpected("Renderer, Debug-Linien: " + debug_lines.error());
    }
    impl->debug_lines = *std::move(debug_lines);
    auto tonemap = device.create_pipeline({.vertex = impl->fullscreen_vertex.get(),
                                           .fragment = impl->tonemap_fragment.get(),
                                           .color_format = target_format});
    if (!tonemap) {
        return std::unexpected("Renderer, Tone Mapping: " + tonemap.error());
    }
    impl->tonemap = *std::move(tonemap);
    for (usize i = 0; i < impl->id_pipelines.size(); ++i) {
        auto pipeline = device.create_pipeline(
            {.vertex = impl->id_vertex.get(),
             .fragment = impl->id_fragment.get(),
             .color_format = id_format,
             .depth_format = depth_format,
             .depth_compare = rhi::CompareOp::Greater,
             .cull_mode = i == 0 ? rhi::CullMode::Back : rhi::CullMode::None});
        if (!pipeline) {
            return std::unexpected("Renderer, ID-Puffer: " + pipeline.error());
        }
        impl->id_pipelines[i] = *std::move(pipeline);
    }

    // Gather liest vier Texel ohne Filter; nur Klemmen am Rand zählt.
    auto sampler = device.create_sampler({.mag_filter = rhi::Filter::Nearest,
                                          .min_filter = rhi::Filter::Nearest,
                                          .mip_filter = rhi::Filter::Nearest,
                                          .address_u = rhi::AddressMode::ClampToEdge,
                                          .address_v = rhi::AddressMode::ClampToEdge});
    if (!sampler) {
        return std::unexpected("Renderer, Sampler der Schatten: " + sampler.error());
    }
    impl->shadow_sampler = *std::move(sampler);

    const auto buffer = [&](u64 size) -> rhi::Result<std::unique_ptr<rhi::Buffer>> {
        return device.create_buffer({.size = size,
                                     .usage = rhi::BufferUsage::Storage,
                                     .memory = rhi::MemoryUsage::CpuWrite});
    };
    for (u32 i = 0; i < std::max(device.frames_in_flight(), 1u); ++i) {
        auto frame = buffer(sizeof(GpuFrame));
        auto lights = buffer(u64{max_lights} * sizeof(GpuLight));
        if (!frame || !lights) {
            return std::unexpected("Renderer, Frame-Puffer: " +
                                   (frame ? lights.error() : frame.error()));
        }
        impl->frames.push_back(*std::move(frame));
        impl->lights.push_back(*std::move(lights));
        impl->lines.emplace_back();
        auto pick = device.create_buffer({.size = sizeof(u32),
                                          .usage = rhi::BufferUsage::Storage,
                                          .memory = rhi::MemoryUsage::CpuRead});
        if (!pick) {
            return std::unexpected("Renderer, Puffer der Auswahl: " + pick.error());
        }
        impl->pick_buffers.push_back(*std::move(pick));
        impl->pick_pending.emplace_back();
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
DebugDraw& Renderer::debug() noexcept {
    return impl_->debug;
}
const RenderStats& Renderer::stats() const noexcept {
    return impl_->stats;
}

void Renderer::request_pick(u32 x, u32 y) {
    impl_->pick_requested = PickRequest{.x = x, .y = y};
}

std::optional<PickResult> Renderer::take_pick(const world::World& world) {
    if (!impl_->pick_answered) {
        return std::nullopt;
    }
    const PickResult result{.x = impl_->pick_answered->x,
                            .y = impl_->pick_answered->y,
                            .entity = world.entity_from_index(impl_->pick_answer)};
    impl_->pick_answered.reset();
    return result;
}

void Renderer::add_passes(RenderGraph& graph, world::World& world, TextureHandle target) {
    NGINE_PROFILE_ZONE("Renderer::add_passes");
    Impl& r = *impl_;
    const rhi::Extent2D extent = graph.texture_desc(target).extent;
    verify(graph.texture_desc(target).format == r.target_format,
           "Renderer::add_passes: Ziel hat nicht das Format aus create()");
    const f32 aspect =
        static_cast<f32>(extent.width) / static_cast<f32>(std::max(extent.height, 1u));

    // Puffer dieses Frames: frei, weil reihum so viele benutzt werden wie Frames im Flug.
    const usize slot = r.next_frame;
    r.next_frame = (r.next_frame + 1) % r.frames.size();
    rhi::Buffer& frame_buffer = *r.frames[slot];
    rhi::Buffer& light_buffer = *r.lights[slot];
    r.stats = {};

    // Antwort einer Auswahl, die dieser Platz vor frames_in_flight Frames angefragt hat; die GPU
    // ist damit fertig.
    if (r.pick_pending[slot]) {
        u32 id = 0;
        r.pick_buffers[slot]->read(0, std::as_writable_bytes(std::span{&id, 1}));
        r.pick_answered = r.pick_pending[slot];
        r.pick_answer = id;
        r.pick_pending[slot].reset();
    }

    const std::optional<CameraView> camera = camera_override ? camera_override : find_camera(world);
    const math::DVec3 origin = camera ? camera->position : math::DVec3{};
    const Vec3 forward = camera ? -camera->rotation[2] : Vec3{0.0f, 0.0f, -1.0f};

    // Lichter und Sonne.
    const LightList lights = collect_lights(world, origin);
    if (!lights.lights.empty()) {
        light_buffer.write(0, std::as_bytes(std::span{lights.lights}));
    }
    r.stats.lights = static_cast<u32>(lights.lights.size()) + (lights.sun ? 1u : 0u);
    const bool shadows_on = camera && shadows.enabled && lights.sun && lights.sun->cast_shadows;
    const u32 resolution = std::clamp(shadows.resolution, 16u, 8192u);
    std::array<Cascade, cascade_count> cascades{};
    if (shadows_on) {
        ShadowSettings settings = shadows;
        settings.resolution = resolution;
        cascades = fit_cascades({.position = camera->position,
                                 .rotation = camera->rotation,
                                 .fov_y = camera->camera.fov_y,
                                 .aspect = aspect,
                                 .near_plane = camera->camera.near_plane},
                                lights.sun->to_sun, settings);
        if (shadows.depth_bias_constant != r.shadow_bias_constant ||
            shadows.depth_bias_slope != r.shadow_bias_slope) {
            if (auto result = r.create_shadow_pipelines(shadows.depth_bias_constant,
                                                        shadows.depth_bias_slope);
                !result) {
                log::error("{}", result.error());
            }
        }
    }

    // Frame-Daten: Kamera, Licht, Kaskaden. Die Indizes der Schattenkarten kennt erst der Pass.
    {
        const Mat4 rows = camera ? math::transpose(view_projection(*camera, aspect)) : Mat4{};
        GpuFrame frame{
            .view_projection = {rows[0], rows[1], rows[2], rows[3]},
            .sun_direction =
                math::extend(lights.sun ? lights.sun->to_sun : Vec3{0.0f, 1.0f, 0.0f}, 0.0f),
            .sun_color = math::extend(lights.sun ? lights.sun->radiance : Vec3{}, 0.0f),
            .sky_color = math::extend(lighting.sky_color, 1.0f),
            .ground_color = math::extend(lighting.ground_color, 1.0f),
            .camera_forward = math::extend(forward, 0.0f),
        };
        for (u32 i = 0; i < cascade_count; ++i) {
            frame.cascade_far[i] = cascades[i].split_far;
            frame.cascade_blend[i] = cascades[i].blend_start;
            frame.cascade_texel[i] = cascades[i].texel_size;
            const Mat4 cascade_rows = math::transpose(cascades[i].view_projection);
            for (u32 row = 0; row < 3; ++row) {
                frame.cascade_rows[i * 3 + row] = cascade_rows[row];
            }
            frame.shadow_maps[i] = rhi::invalid_index;
        }
        frame.options[0] = static_cast<u32>(lights.lights.size());
        frame.options[1] = shadows_on ? 1u : 0u;
        frame.options[2] = show_cascades ? 1u : 0u;
        frame.options[3] = r.shadow_sampler->index();
        frame.shadow_params = {shadows.normal_offset, 1.0f / static_cast<f32>(resolution),
                               static_cast<f32>(resolution), 0.0f};
        frame.lights = light_buffer.device_address();
        frame_buffer.write(0, std::as_bytes(std::span{&frame, 1}));
    }
    r.materials.flush();

    // Draws sammeln, undurchsichtige zuerst; dazu die Werfer jeder Kaskade.
    r.draws.clear();
    for (auto& list : r.shadow_draws) {
        list.clear();
    }
    if (camera) {
        world.each<const world::WorldTransform, const MeshRenderer>(
            [&](world::Entity entity, const world::WorldTransform& transform,
                const MeshRenderer& renderer) {
                const GpuMesh* mesh = r.meshes.get(renderer.mesh);
                if (mesh == nullptr || !mesh->indices) {
                    return;
                }
                const auto id = static_cast<u32>(entity.id());
                const Mat4 rows = math::transpose(transform.value.relative_to(origin));
                const std::array<Vec4, 3> model{rows[0], rows[1], rows[2]};
                std::array<bool, cascade_count> casts{};
                if (shadows_on) {
                    const auto [center, radius] = bounding_sphere(*mesh, transform.value, origin);
                    for (u32 i = 0; i < cascade_count; ++i) {
                        casts[i] = cascades[i].may_cast(center, radius);
                    }
                }
                for (const Submesh& submesh : mesh->submeshes) {
                    u32 material = renderer.material != rhi::invalid_index ? renderer.material
                                                                           : submesh.material;
                    if (material >= r.materials.size()) {
                        material = 0;
                    }
                    const Material& properties = r.materials.get(material);
                    r.draws.push_back({.model = model,
                                       .mesh = mesh,
                                       .first_index = submesh.first_index,
                                       .index_count = submesh.index_count,
                                       .material = material,
                                       .pipeline = variant(properties),
                                       .id = id});
                    if (properties.alpha_mode == AlphaMode::Blend) {
                        continue;
                    }
                    for (u32 i = 0; i < cascade_count; ++i) {
                        if (casts[i]) {
                            r.shadow_draws[i].push_back(
                                {.model = model,
                                 .mesh = mesh,
                                 .first_index = submesh.first_index,
                                 .index_count = submesh.index_count,
                                 .material = material,
                                 .masked = properties.alpha_mode == AlphaMode::Mask});
                        }
                    }
                }
            });
        std::ranges::stable_partition(r.draws, [](const Draw& draw) { return draw.pipeline < 2; });
    }

    // Debug-Linien relativ zur Kamera; die Liste gilt nur für diesen Frame.
    r.line_count = 0;
    u64 lines_address = 0;
    if (camera && !r.debug.lines().empty()) {
        const std::vector<GpuDebugLine> prepared =
            prepare_lines(r.debug.lines(), origin, forward, camera->camera.near_plane);
        if (!prepared.empty()) {
            if (auto buffer = r.line_buffer(slot, prepared.size())) {
                (*buffer)->write(0, std::as_bytes(std::span{prepared}));
                lines_address = (*buffer)->device_address();
                r.line_count = static_cast<u32>(prepared.size());
            } else {
                log::error("Renderer, Puffer der Debug-Linien: {}", buffer.error());
            }
        }
    }
    r.stats.debug_lines = r.line_count;
    r.debug.clear();

    const u64 frame_address = frame_buffer.device_address();

    // Schattenkarten der Sonne, eine pro Kaskade.
    std::array<TextureHandle, cascade_count> shadow_maps{};
    if (shadows_on) {
        for (u32 i = 0; i < cascade_count; ++i) {
            shadow_maps[i] = graph.create_texture(
                "Schatten", {.extent = {resolution, resolution}, .format = depth_format});
            const TextureHandle map = shadow_maps[i];
            graph.add_pass(
                "Schatten", [&](PassBuilder& pass) { pass.write(map, ResourceState::DepthTarget); },
                [this, map, i, frame_address](PassContext& context) {
                    Impl& r = *impl_;
                    context.commands.begin_rendering(
                        {.depth = &context.texture(map), .clear_depth = 1.0f});
                    const rhi::Pipeline* bound = nullptr;
                    const rhi::Buffer* indices = nullptr;
                    for (const ShadowDraw& draw : r.shadow_draws[i]) {
                        const rhi::Pipeline* pipeline =
                            draw.masked ? r.shadow_masked.get() : r.shadow_opaque.get();
                        if (bound != pipeline) {
                            bound = pipeline;
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
                                            .material = draw.material,
                                            .cascade = i};
                        context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                        context.commands.draw_indexed(draw.index_count, 1, draw.first_index);
                        ++r.stats.shadow_draws;
                    }
                    context.commands.end_rendering();
                });
        }
    }

    const TextureHandle hdr = graph.create_texture("HDR", {.extent = extent, .format = hdr_format});
    const TextureHandle depth =
        graph.create_texture("Tiefe", {.extent = extent, .format = depth_format});
    graph.add_pass(
        "Vorwärts",
        [&](PassBuilder& pass) {
            pass.write(hdr, ResourceState::ColorTarget);
            pass.write(depth, ResourceState::DepthTarget);
            if (shadows_on) {
                for (const TextureHandle map : shadow_maps) {
                    pass.read(map, ResourceState::ShaderRead);
                }
            }
        },
        [this, hdr, depth, frame_address, shadows_on, shadow_maps, extent, lines_address,
         &frame_buffer](PassContext& context) {
            Impl& r = *impl_;
            if (shadows_on) {
                std::array<u32, cascade_count> indices{};
                for (u32 i = 0; i < cascade_count; ++i) {
                    indices[i] = context.texture(shadow_maps[i]).sampled_index();
                }
                frame_buffer.write(offsetof(GpuFrame, shadow_maps),
                                   std::as_bytes(std::span{indices}));
            }
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
            if (r.line_count > 0) {
                const DebugPush push{
                    .lines = lines_address,
                    .frame = frame_address,
                    .viewport = {static_cast<f32>(extent.width), static_cast<f32>(extent.height)},
                    .width = r.debug.width};
                context.commands.bind_pipeline(*r.debug_lines);
                context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                context.commands.draw(r.line_count * 6);
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

    // Auswahl per Klick: ID-Puffer in Größe des Ziels und ein Pixel davon in den Puffer dieses
    // Platzes. Ohne Kamera oder außerhalb des Ziels ist die Antwort sofort „nichts“.
    if (r.pick_requested) {
        const PickRequest request = *r.pick_requested;
        r.pick_requested.reset();
        if (!camera || request.x >= extent.width || request.y >= extent.height) {
            r.pick_answered = request;
            r.pick_answer = 0;
        } else {
            r.add_pick_passes(graph, slot, request, extent, frame_address);
        }
    }
}

} // namespace ngine::render
