module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Ablauf eines Frames im Renderer: Kamera, Lichter, Kaskaden, Draws sammeln, Passes anlegen. Die
// Passes selbst stehen in passes.cpp, Shader und Pipelines in pipelines.cpp, das Terrain in
// terrain_pass.cpp (TD-36).
module ngine.render;

import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.rhi;
import ngine.asset;
import ngine.world;
import :renderer_detail;

namespace ngine::render {

namespace {

using math::Mat4;
using math::Vec3;
using math::Vec4;
using rhi::ResourceState;

// Pipeline-Variante je Material: durchsichtig oder nicht, doppelseitig oder nicht.
usize variant(const Material& material) {
    return (material.alpha_mode == AlphaMode::Blend ? 2u : 0u) + (material.double_sided ? 1u : 0u);
}

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

// --- Renderer::Impl -------------------------------------------------------------------------

// Der Zustand steht in RendererCore (Partition :renderer_detail), damit die Dateien des Renderers
// ihn teilen. Die verschachtelte Klasse selbst wird nur hier vervollständigt: Als Definition in
// einer Partition brach MSVC beim Lesen des Moduls mit einem internen Fehler ab (C1001).
struct Renderer::Impl : RendererCore {
    using RendererCore::RendererCore;
};

const GpuModel* RendererCore::model(asset::AssetId id) {
    if (id == 0 || resources == nullptr) {
        return nullptr;
    }
    auto [it, inserted] = models.try_emplace(id);
    if (inserted || !it->second.handle) {
        it->second.handle = resources->load<GpuModel>(id);
    }
    it->second.last_used = frame_number;
    const GpuModel* found = it->second.handle.get();
    // Uploads im Hintergrund noch nicht fertig (TD-17): wie nicht geladen behandeln.
    if (found != nullptr && !device->upload_done(found->ticket())) {
        return nullptr;
    }
    return found;
}

const GpuMesh* RendererCore::mesh(const asset::AssetRef& ref) {
    const GpuModel* found = model(ref.asset);
    if (found == nullptr || ref.index >= found->instance().meshes.size()) {
        return nullptr;
    }
    return meshes.get(found->instance().meshes[ref.index]);
}

std::optional<u32> RendererCore::material(const asset::AssetRef& ref) {
    if (!ref.valid()) {
        return std::nullopt;
    }
    const GpuModel* found = model(ref.asset);
    if (found == nullptr || ref.index >= found->instance().materials.size()) {
        return std::nullopt;
    }
    return found->instance().materials[ref.index];
}

rhi::Result<rhi::Buffer*> RendererCore::line_buffer(usize slot, usize count) {
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

void RendererCore::collect_meshes(world::World& world, math::DVec3 origin) {
    world.each<const world::WorldTransform, const MeshRenderer>(
        [&](world::Entity entity, const world::WorldTransform& transform,
            const MeshRenderer& renderer) {
            const GpuMesh* found = mesh(renderer.mesh);
            if (found == nullptr) {
                ++stats.waiting;
                return;
            }
            if (found->index_buffer == nullptr) {
                return;
            }
            const std::optional<u32> override_material = material(renderer.material);
            const auto id = static_cast<u32>(entity.id());
            const Mat4 rows = math::transpose(transform.value.relative_to(origin));
            const auto [center, radius] = bounding_sphere(*found, transform.value, origin);
            for (const Submesh& submesh : found->submeshes) {
                if (submesh.meshlet_count == 0) {
                    continue;
                }
                u32 index = override_material.value_or(submesh.material);
                if (index >= materials.size()) {
                    index = 0;
                }
                const Material& properties = materials.get(index);
                const bool blend = properties.alpha_mode == AlphaMode::Blend;
                const bool masked = properties.alpha_mode == AlphaMode::Mask;
                const auto category =
                    properties.double_sided
                        ? (masked ? detail::Category::MaskedDoubleSided
                                  : detail::Category::OpaqueDoubleSided)
                        : (masked ? detail::Category::Masked : detail::Category::Opaque);
                const auto instance_index = static_cast<u32>(instances.size());
                instances.push_back(
                    {.model = {rows[0], rows[1], rows[2]},
                     .sphere = math::extend(center, radius),
                     .vertices = found->vertex_address,
                     .meshlets = found->meshlet_address,
                     .words = found->meshlet_address,
                     .first_meshlet = submesh.first_meshlet,
                     .meshlet_count = submesh.meshlet_count,
                     .material = index,
                     .id = id,
                     .first_index = submesh.first_index,
                     .flags = properties.double_sided ? instance_double_sided : 0u});
                instance_keys.push_back({.category = category,
                                         .block = found->indices.block,
                                         .index_buffer = found->index_buffer,
                                         .blend = blend});
                if (blend) {
                    blend_draws.push_back({.instance = instance_index,
                                           .index_buffer = found->index_buffer,
                                           .first_index = submesh.first_index,
                                           .index_count = submesh.index_count,
                                           .pipeline = variant(properties),
                                           .distance = math::length(center)});
                }
                ++stats.draws;
                stats.triangles += submesh.index_count / 3;
            }
        });
}

// --- Renderer -------------------------------------------------------------------------------

rhi::Result<std::unique_ptr<Renderer>> Renderer::create(rhi::Device& device,
                                                        rhi::Format target_format) {
    auto impl = std::make_unique<Impl>(device);
    impl->target_format = target_format;

    if (auto result = impl->create_shaders(); !result) {
        return std::unexpected(result.error());
    }
    const ShadowSettings defaults;
    impl->shadow_bias_constant = defaults.depth_bias_constant;
    impl->shadow_bias_slope = defaults.depth_bias_slope;
    if (auto result = impl->create_pipelines(); !result) {
        return std::unexpected(result.error());
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
    // Texturen des Terrains wiederholen sich und werden schräg betrachtet.
    auto terrain_sampler = impl->materials.sampler({.anisotropy = 8.0f});
    if (!terrain_sampler) {
        return std::unexpected("Renderer, Sampler des Terrains: " + terrain_sampler.error());
    }
    impl->terrain_sampler = *terrain_sampler;

    const auto buffer = [&](u64 size) -> rhi::Result<std::unique_ptr<rhi::Buffer>> {
        return device.create_buffer({.size = size,
                                     .usage = rhi::BufferUsage::Storage,
                                     .memory = rhi::MemoryUsage::CpuWrite});
    };
    for (u32 i = 0; i < std::max(device.frames_in_flight(), 1u); ++i) {
        auto frame = buffer(sizeof(GpuFrame));
        auto lights = buffer(u64{max_lights} * sizeof(GpuLight));
        auto terrain_materials = buffer(u64{max_terrain_materials} * sizeof(GpuTerrainMaterial));
        if (!frame || !lights || !terrain_materials) {
            return std::unexpected("Renderer, Frame-Puffer: " + (!frame ? frame.error()
                                                                 : !lights
                                                                     ? lights.error()
                                                                     : terrain_materials.error()));
        }
        impl->frames.push_back(*std::move(frame));
        impl->lights.push_back(*std::move(lights));
        impl->terrain_material_buffers.push_back(*std::move(terrain_materials));
        impl->lines.emplace_back();
        auto pick = device.create_buffer({.size = sizeof(u32),
                                          .usage = rhi::BufferUsage::Storage,
                                          .memory = rhi::MemoryUsage::CpuRead});
        if (!pick) {
            return std::unexpected("Renderer, Puffer der Auswahl: " + pick.error());
        }
        impl->pick_buffers.push_back(*std::move(pick));
        impl->pick_pending.emplace_back();
        impl->driven.emplace_back();
    }
    return std::unique_ptr<Renderer>{new Renderer{std::move(impl)}};
}

Renderer::Renderer(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Renderer::~Renderer() {
    // Handles zuerst: Ein letztes GpuModel gibt seinen Speicher in Meshes und Materialien frei,
    // ebenso die Texturen des Terrains.
    impl_->models.clear();
    impl_->color_textures.clear();
    impl_->data_textures.clear();
}

MeshStore& Renderer::meshes() noexcept {
    return impl_->meshes;
}
GeometryBuffer& Renderer::geometry() noexcept {
    return impl_->geometry;
}
MaterialStore& Renderer::materials() noexcept {
    return impl_->materials;
}
rhi::Device& Renderer::device() noexcept {
    return *impl_->device;
}

void Renderer::set_resources(asset::ResourceManager* resources) noexcept {
    impl_->resources = resources;
    impl_->models.clear();
    impl_->color_textures.clear();
    impl_->data_textures.clear();
}
asset::ResourceManager* Renderer::resources() noexcept {
    return impl_->resources;
}
const GpuMesh* Renderer::find_mesh(const asset::AssetRef& ref) {
    return impl_->mesh(ref);
}
const GpuModel* Renderer::find_model(asset::AssetId id) {
    return impl_->model(id);
}

void Renderer::set_terrain_materials(std::vector<TerrainMaterial> materials) {
    if (materials.size() > max_terrain_materials) {
        log::warn("Renderer: {} Materialien des Terrains, höchstens {} gelten", materials.size(),
                  max_terrain_materials);
        materials.resize(max_terrain_materials);
    }
    impl_->terrain_materials = std::move(materials);
}
const std::vector<TerrainMaterial>& Renderer::terrain_materials() const noexcept {
    return impl_->terrain_materials;
}

rhi::Result<void> Renderer::reload_shaders(const ShaderSource& source) {
    return impl_->reload_shaders(source);
}
DebugDraw& Renderer::debug() noexcept {
    return impl_->debug;
}
const RenderStats& Renderer::stats() const noexcept {
    return impl_->stats;
}

void Renderer::request_pick(u32 x, u32 y) {
    impl_->pick_requested = detail::PickRequest{.x = x, .y = y};
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
    // Modelle, die zwei Frames lang niemand brauchte, loslassen.
    ++r.frame_number;
    r.geometry.end_frame();
    std::erase_if(r.models,
                  [&](const auto& entry) { return entry.second.last_used + 2 < r.frame_number; });

    // Antwort einer Auswahl, die dieser Platz vor frames_in_flight Frames angefragt hat; die GPU
    // ist damit fertig.
    if (r.pick_pending[slot]) {
        u32 id = 0;
        r.pick_buffers[slot]->read(0, std::as_writable_bytes(std::span{&id, 1}));
        r.pick_answered = r.pick_pending[slot];
        r.pick_answer = id;
        r.pick_pending[slot].reset();
    }
    // Statistik der GPU aus demselben Platz.
    r.read_driven_stats(slot);
    r.stats.meshlets_drawn = r.gpu_stats[4];
    r.stats.triangles_drawn = r.gpu_stats[5];
    r.stats.culled_frustum = r.gpu_stats[1];
    r.stats.culled_backface = r.gpu_stats[2];
    r.stats.culled_occluded = r.gpu_stats[3];

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

    // Instanzen sammeln und zu Gruppen von Meshlets ordnen (R3.5).
    r.instances.clear();
    r.instance_keys.clear();
    r.blend_draws.clear();
    if (camera) {
        r.collect_terrain(world, origin);
        r.collect_meshes(world, origin);
    }
    r.build_groups();
    r.stats.instances = static_cast<u32>(r.instances.size());
    const u64 terrain_materials = r.write_terrain_materials(slot, r.stats.terrain_chunks > 0);

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

    // Pfad und Hi-Z dieses Frames.
    const bool mesh_shaders = use_mesh_shaders && r.mesh_capable();
    r.stats.mesh_shaders = mesh_shaders;
    const bool occlusion = camera.has_value() && occlusion_culling && r.ensure_hiz(extent);
    const Mat4 camera_matrix = camera ? view_projection(*camera, aspect) : Mat4{};
    const bool previous_hiz = occlusion && r.previous_valid;
    const u32 hiz_current = r.hiz_next;
    const u32 hiz_previous = 1 - hiz_current;
    const u32 hiz_levels = occlusion ? static_cast<u32>(r.hiz_sets[hiz_current].levels.size()) : 0;

    // Sichten: Kamera (zwei Phasen), Kaskaden, ID-Puffer.
    std::array<GpuView, detail::job_count> views{};
    const auto fill_view = [&](GpuView& view, const Mat4& matrix, u32 flags) {
        const std::array<Vec4, 6> planes = frustum_planes(matrix);
        std::ranges::copy(planes, std::begin(view.planes));
        const Mat4 rows = math::transpose(matrix);
        for (u32 i = 0; i < 4; ++i) {
            view.view_projection[i] = rows[i];
        }
        view.params[0] = flags;
    };
    fill_view(views[0], camera_matrix, view_cone | (previous_hiz ? view_previous_hiz : 0u));
    if (occlusion) {
        const Mat4 previous_rows = math::transpose(r.previous_view_projection);
        for (u32 i = 0; i < 4; ++i) {
            views[0].previous_view_projection[i] = previous_rows[i];
        }
        views[0].previous_offset = math::extend(math::to_f32(origin - r.previous_origin), 0.0f);
        for (u32 level = 0; level < hiz_levels; ++level) {
            views[0].hiz_current[level] = r.hiz_sets[hiz_current].levels[level]->sampled_index();
            views[0].hiz_previous[level] = r.hiz_sets[hiz_previous].levels[level]->sampled_index();
        }
        views[0].params[1] = hiz_levels;
        views[0].params[2] = r.hiz_size.width;
        views[0].params[3] = r.hiz_size.height;
    }
    views[1] = views[0];
    views[1].params[0] |= view_phase_two;
    for (u32 i = 0; i < cascade_count; ++i) {
        fill_view(views[2 + i], cascades[i].view_projection, 0);
    }
    fill_view(views[6], camera_matrix, view_cone);
    const bool driven_ok = r.write_driven(slot, views, mesh_shaders);

    const detail::FrameInfo frame{.frame_buffer = &frame_buffer,
                                  .frame_address = frame_buffer.device_address(),
                                  .shadows_on = shadows_on,
                                  .shadow_resolution = resolution,
                                  .extent = extent,
                                  .lines_address = lines_address,
                                  .sky_color = lighting.sky_color,
                                  .exposure = camera ? camera->camera.exposure : 1.0f,
                                  .terrain_materials = terrain_materials,
                                  .driven = &r.driven[slot],
                                  .mesh_shaders = mesh_shaders,
                                  .occlusion = occlusion && driven_ok,
                                  .hiz_current = hiz_current,
                                  .hiz_extent = r.hiz_size,
                                  .hiz_levels = hiz_levels};
    if (!driven_ok) {
        // Ohne Puffer nichts zeichnen; Himmel und Linien bleiben.
        r.groups.clear();
        r.regions.clear();
        r.category_groups = {};
        r.blend_draws.clear();
    }

    // Puffer und Hi-Z im Graph. Die Stufen des Hi-Z leben über Frames; nach dem ersten Bau sind
    // sie im Zustand ShaderRead.
    RendererCore::DrivenHandles handles;
    detail::DrivenBuffers& buffers = r.driven[slot];
    const bool have_buffers = buffers.counters != nullptr;
    if (have_buffers) {
        handles.counters = graph.import_buffer("Zähler", *buffers.counters,
                                               ResourceState::Undefined, ResourceState::Undefined);
        if (!mesh_shaders) {
            handles.commands = graph.import_buffer(
                "Befehle", *buffers.commands, ResourceState::Undefined, ResourceState::Undefined);
        }
    }
    if (frame.occlusion) {
        const auto import_set = [&](detail::HizSet& set, std::vector<TextureHandle>& handles_out) {
            const ResourceState initial =
                set.valid ? ResourceState::ShaderRead : ResourceState::Undefined;
            for (const auto& level : set.levels) {
                handles_out.push_back(
                    graph.import_texture("Hi-Z", *level, initial, ResourceState::ShaderRead));
            }
        };
        import_set(r.hiz_sets[hiz_current], handles.hiz_current);
        if (previous_hiz) {
            import_set(r.hiz_sets[hiz_previous], handles.hiz_previous);
        }
    }

    std::array<TextureHandle, cascade_count> shadow_maps{};
    TextureHandle hdr;
    if (have_buffers && driven_ok) {
        r.add_clear_pass(graph, frame, handles.counters);
        if (!mesh_shaders) {
            std::vector<detail::Job> jobs{detail::Job::Forward};
            if (shadows_on) {
                jobs.insert(jobs.end(), {detail::Job::Shadow0, detail::Job::Shadow1,
                                         detail::Job::Shadow2, detail::Job::Shadow3});
            }
            if (r.pick_requested) {
                jobs.push_back(detail::Job::Pick);
            }
            r.add_cull_pass(graph, "Prüfen", frame, jobs, handles.commands, handles.counters,
                            handles.hiz_previous);
        }
        shadow_maps = r.add_shadow_passes(graph, frame, handles);
        hdr = r.add_forward_passes(graph, frame, shadow_maps, handles);
    } else {
        const detail::FrameInfo empty_frame = [&] {
            detail::FrameInfo copy = frame;
            copy.shadows_on = false;
            copy.occlusion = false;
            return copy;
        }();
        hdr = r.add_forward_passes(graph, empty_frame, shadow_maps, handles);
    }
    r.add_tonemap_pass(graph, frame, hdr, target);

    // Auswahl per Klick: ID-Puffer in Größe des Ziels und ein Pixel davon in den Puffer dieses
    // Platzes. Ohne Kamera oder außerhalb des Ziels ist die Antwort sofort „nichts“.
    if (r.pick_requested) {
        const detail::PickRequest request = *r.pick_requested;
        r.pick_requested.reset();
        if (!camera || request.x >= extent.width || request.y >= extent.height || !have_buffers ||
            !driven_ok) {
            r.pick_answered = request;
            r.pick_answer = 0;
        } else {
            r.add_pick_passes(graph, slot, request, frame, handles);
        }
    }
    if (have_buffers && driven_ok) {
        r.add_stats_pass(graph, frame, handles.counters);
    }

    // Hi-Z dieses Frames gilt im nächsten als das des letzten.
    if (frame.occlusion) {
        r.hiz_sets[hiz_current].valid = true;
        r.hiz_next = hiz_previous;
        r.previous_view_projection = camera_matrix;
        r.previous_origin = origin;
        r.previous_valid = true;
    } else {
        r.previous_valid = false;
    }
}

} // namespace ngine::render
