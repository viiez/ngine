module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Voxel-Terrain in der Sandbox. Seit R3.1 ein Terrain aus mehreren Chunks als Komponente der
// Welt, gemeshed im Job-System und gezeichnet vom Renderer mit PBR und Schatten (vorher, im
// Durchstich D1, ein Chunk mit eigenem Pass). Mit der Maus veränderbar, auch über Chunk-Grenzen.
export module sandbox.voxel_demo;

import ngine.core;
import ngine.filesystem;
import ngine.jobs;
import ngine.math;
import ngine.profile;
import ngine.reflect;
import ngine.render;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.voxel;
import ngine.world;

namespace sandbox {

using namespace ngine;
namespace {

using math::DVec3;
using math::Quat;
using math::Vec3;

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    return types;
}

// Strahl durch einen Bildpunkt (Ursprung oben links) einer Kamera mit Drehung rotation.
Vec3 camera_ray(const math::Mat3& rotation, f32 fov_y, f32 x, f32 y, rhi::Extent2D extent) {
    const f32 aspect = static_cast<f32>(extent.width) / static_cast<f32>(extent.height);
    const f32 t = std::tan(fov_y * 0.5f);
    const f32 nx = 2.0f * x / static_cast<f32>(extent.width) - 1.0f;
    const f32 ny = 1.0f - 2.0f * y / static_cast<f32>(extent.height);
    return math::normalize(-rotation[2] + rotation[0] * (nx * t * aspect) + rotation[1] * (ny * t));
}

f64 elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

// Zustand der Demo: Welt mit Terrain, Sonne und Kamera, Pinsel, Renderer.
export class VoxelScene {
public:
    // origin: Lage des Terrains in der Welt. Weit vom Ursprung zeigt die Demo, dass nichts
    // zittert, weil relativ zur Kamera gezeichnet wird (--far).
    explicit VoxelScene(DVec3 origin = DVec3{0, 0, 0}, world::Terrain terrain = {})
        : types_{make_types()}, world_{types_}, origin_{origin} {
        terrain_ = world_.create("Terrain");
        terrain_.set(world::Transform{.position = origin}).set(terrain);
        world_.create("Sonne")
            .set(world::Transform{.rotation = math::look_rotation(
                                      math::normalize(Vec3{0.5f, -0.7f, -0.45f}), {0, 1, 0})})
            .set(render::DirectionalLight{.color = {1.0f, 0.95f, 0.88f}, .intensity = 3.0f});
        camera_ = world_.create("Kamera");
        camera_.set(world::Transform{}).set(render::Camera{.near_plane = 0.2f});
        // Mitte der Oberfläche: Hier treffen sich vier Chunks, Änderungen kreuzen ihre Grenzen.
        center_ = {0.0f, terrain.ground, 0.0f};
        distance_ = static_cast<f32>(std::max(terrain.size_x, terrain.size_z)) * 16.0f;
        update_camera();
        remesh();
    }

    // --- Eingabe ------------------------------------------------------------------------------

    void set_cursor(f32 x, f32 y) {
        cursor_ = {x, y};
    }
    // Pinsel an der Mausposition, solange eine Taste gedrückt ist; nullopt hört auf.
    void set_brush(std::optional<voxel::BrushMode> mode) {
        brush_ = mode;
        brush_timer_ = 0.0f;
    }
    void zoom(f32 steps) {
        distance_ = std::clamp(distance_ * std::pow(0.9f, steps), 5.0f, 300.0f);
    }
    void toggle_rotation() {
        rotating_ = !rotating_;
    }

    // --- Ablauf -------------------------------------------------------------------------------

    // Bewegt die Kamera und wendet den gehaltenen Pinsel an (30-mal pro Sekunde).
    void update(f32 dt, rhi::Extent2D viewport) {
        if (rotating_) {
            yaw_ += dt * 0.15f;
        }
        update_camera();
        if (!brush_ || !cursor_) {
            return;
        }
        brush_timer_ -= dt;
        if (brush_timer_ > 0.0f) {
            return;
        }
        brush_timer_ = 1.0f / 30.0f;
        const Vec3 direction = camera_ray(math::to_mat3(rotation_), render::Camera{}.fov_y,
                                          cursor_->first, cursor_->second, viewport);
        if (const auto hit = voxel::raycast(chunks(), eye_in_terrain(), direction, 1000.0f)) {
            edit({.mode = *brush_,
                  .center = hit->position,
                  .radius = 4.0f,
                  .material = 2,
                  .strength = 0.6f});
        }
    }

    // Automatische Änderung für Tests und die CI: Abtragen auf einem Kreis um die Mitte (dort
    // treffen sich vier Chunks), jeder fünfte Schritt trägt wieder auf, jeder siebte glättet.
    void scripted_edit(u64 step) {
        const f32 angle = static_cast<f32>(step) * 0.7f;
        const f32 radius = 3.0f + static_cast<f32>(step % 5) * 2.0f;
        const auto meters = static_cast<f32>(voxel::voxel_size);
        const Vec3 above{center_.x / meters + std::cos(angle) * radius, 400.0f,
                         center_.z / meters + std::sin(angle) * radius};
        const auto hit = voxel::raycast(chunks(), above, {0.0f, -1.0f, 0.0f}, 800.0f);
        if (!hit) {
            return;
        }
        voxel::BrushMode mode = voxel::BrushMode::Remove;
        if (step % 7 == 6) {
            mode = voxel::BrushMode::Smooth;
        } else if (step % 5 == 4) {
            mode = voxel::BrushMode::Add;
        }
        edit({.mode = mode,
              .center = hit->position,
              .radius = 4.0f,
              .material = 2,
              .strength = 0.8f});
    }

    void edit(const voxel::Brush& brush) {
        static_cast<void>(voxel::apply(chunks(), brush));
        ++edits_;
    }

    // Meshed geänderte Chunks neu (im Job-System). Liefert die Zeit in Millisekunden, wenn es
    // etwas zu tun gab.
    std::optional<f64> remesh_if_needed() {
        if (!chunks_ready() || chunks().has_changes()) {
            return remesh();
        }
        return std::nullopt;
    }

    f64 remesh() {
        NGINE_PROFILE_ZONE("VoxelScene::remesh");
        const auto start = std::chrono::steady_clock::now();
        const usize meshed = world::update_terrains(world_, jobs_);
        const f64 ms = elapsed_ms(start);
        if (meshed > 0) {
            ++mesh_count_;
            total_ms_ += ms;
            max_ms_ = std::max(max_ms_, ms);
            meshed_chunks_ += meshed;
        }
        return ms;
    }

    [[nodiscard]] const world::TerrainData& data() const {
        return *terrain_.get<world::TerrainState>()->data;
    }
    [[nodiscard]] std::string stats() const {
        const world::TerrainStats& s = data().stats;
        std::string text = std::format(
            "{} Chunks ({} einheitlich, {} KiB Samples), {} Netze mit {} Dreiecken, {} "
            "Änderungen; Meshing im Mittel {:.2f} ms, höchstens {:.2f} ms ({} Läufe, {} Chunks)",
            s.chunks, s.uniform_chunks, s.sample_bytes / 1024, s.meshes, s.triangles, edits_,
            total_ms_ / static_cast<f64>(std::max<u64>(mesh_count_, 1)), max_ms_, mesh_count_,
            meshed_chunks_);
        if (renderer_) {
            const render::RenderStats& r = renderer_->stats();
            text += std::format("; {} Draws ({} Chunks), {} Schatten-Draws", r.draws,
                                r.terrain_chunks, r.shadow_draws);
        }
        return text;
    }

    // --- GPU ----------------------------------------------------------------------------------

    rhi::Result<void> create_gpu(rhi::Device& device, rhi::Format color_format) {
        auto renderer = render::Renderer::create(device, color_format);
        if (!renderer) {
            return std::unexpected(renderer.error());
        }
        renderer_ = *std::move(renderer);
        // Schatten über das ganze Terrain.
        renderer_->shadows.distance = 120.0f;
        return {};
    }

    void release_gpu() {
        renderer_.reset();
    }

    // Fügt dem Graph die Pässe des Renderers hinzu, die die Welt in target zeichnen.
    void add_passes(rhi::Device& /*device*/, render::RenderGraph& graph,
                    render::TextureHandle target) {
        NGINE_PROFILE_ZONE("VoxelScene::add_passes");
        static_cast<void>(world::update_transforms(world_));
        renderer_->add_passes(graph, world_, target);
    }

private:
    [[nodiscard]] bool chunks_ready() const {
        const auto* state = terrain_.get<world::TerrainState>();
        return state != nullptr && state->data;
    }
    [[nodiscard]] voxel::ChunkStore& chunks() {
        return terrain_.get_mut<world::TerrainState>()->data->chunks;
    }

    void update_camera() {
        const f32 pitch = 0.5f;
        const Vec3 offset{std::cos(yaw_) * std::cos(pitch), std::sin(pitch),
                          std::sin(yaw_) * std::cos(pitch)};
        const DVec3 target = origin_ + math::to_f64(center_);
        eye_ = target + math::to_f64(offset * distance_);
        rotation_ = math::look_rotation(math::to_f32(target - eye_), Vec3{0, 1, 0});
        camera_.set(world::Transform{.position = eye_, .rotation = rotation_});
    }

    // Kamera in Voxeln relativ zum Ursprung des Terrains, für den Strahltest.
    [[nodiscard]] Vec3 eye_in_terrain() const {
        return math::to_f32((eye_ - origin_) * (1.0 / voxel::voxel_size));
    }

    reflect::TypeRegistry types_;
    world::World world_;
    jobs::JobSystem jobs_;
    std::unique_ptr<render::Renderer> renderer_;
    world::Entity terrain_;
    world::Entity camera_;

    DVec3 origin_;
    // Mitte der Oberfläche in Metern relativ zu origin_.
    Vec3 center_;
    f32 distance_ = 64.0f;
    f32 yaw_ = 0.6f;
    bool rotating_ = true;
    DVec3 eye_;
    Quat rotation_ = math::quat_identity();

    std::optional<std::pair<f32, f32>> cursor_;
    std::optional<voxel::BrushMode> brush_;
    f32 brush_timer_ = 0.0f;

    u64 edits_ = 0;
    u64 mesh_count_ = 0;
    u64 meshed_chunks_ = 0;
    f64 total_ms_ = 0.0;
    f64 max_ms_ = 0.0;
};

namespace {

// Misst Füllen und Meshing vieler Chunks auf einem Thread und parallel im Job-System.
void benchmark_chunks() {
    const world::Terrain terrain{
        .size_x = 8, .size_y = 4, .size_z = 8, .ground = 24.0f, .height = 10.0f};
    for (const bool parallel : {false, true}) {
        // 0 Worker: Alles läuft im aufrufenden Thread.
        jobs::JobSystem jobs{parallel ? jobs::JobSystemDesc{}
                                      : jobs::JobSystemDesc{.worker_count = 0}};
        voxel::ChunkStore chunks;
        auto start = std::chrono::steady_clock::now();
        world::generate_terrain(terrain, chunks, jobs);
        const f64 fill_ms = elapsed_ms(start);
        const std::vector<voxel::ChunkCoord> coords = chunks.take_changes();
        std::vector<voxel::Mesh> meshes(coords.size());
        // Bestes von drei Läufen.
        f64 mesh_ms = 1e30;
        for (int run = 0; run < 3; ++run) {
            start = std::chrono::steady_clock::now();
            voxel::mesh_chunks(jobs, chunks, coords, meshes);
            mesh_ms = std::min(mesh_ms, elapsed_ms(start));
        }
        usize triangles = 0;
        usize with_mesh = 0;
        usize bytes = 0;
        for (const voxel::Mesh& mesh : meshes) {
            triangles += mesh.triangle_count();
            with_mesh += mesh.indices.empty() ? 0 : 1;
            bytes +=
                mesh.vertices.size() * sizeof(voxel::Vertex) + mesh.indices.size() * sizeof(u32);
        }
        log::info("Chunks {} ({} Threads): {} Chunks gefüllt in {:.1f} ms; {} Netze gemeshed in "
                  "{:.1f} ms ({:.3f} ms pro Netz), {} Dreiecke, {} KiB Netze, {} KiB Samples "
                  "({} von {} Chunks einheitlich)",
                  parallel ? "parallel" : "seriell", jobs.worker_count() + 1, chunks.size(),
                  fill_ms, coords.size(), mesh_ms,
                  mesh_ms / static_cast<f64>(std::max<usize>(with_mesh, 1)), triangles,
                  bytes / 1024, chunks.memory_bytes() / 1024, chunks.uniform_count(),
                  chunks.size());
    }
}

// Schreibt RGBA-Pixel als PPM (Binärformat P6), das jeder Bildbetrachter öffnet.
rhi::Result<void> write_ppm(const std::string& path, std::span<const u8> rgba,
                            rhi::Extent2D extent) {
    const std::string header = std::format("P6\n{} {}\n255\n", extent.width, extent.height);
    std::vector<std::byte> data;
    data.reserve(header.size() + usize{extent.width} * extent.height * 3);
    for (const char c : header) {
        data.push_back(static_cast<std::byte>(c));
    }
    for (usize i = 0; i + 3 < rgba.size(); i += 4) {
        data.push_back(static_cast<std::byte>(rgba[i]));
        data.push_back(static_cast<std::byte>(rgba[i + 1]));
        data.push_back(static_cast<std::byte>(rgba[i + 2]));
    }
    return filesystem::write_file(path, data);
}

} // namespace

// Lage des Terrains in der Welt: im Ursprung oder, mit --far, 10 000 km entfernt. Dort hätte eine
// 32-Bit-Position nur noch eine Auflösung von rund einem Meter.
export DVec3 voxel_origin(bool far) {
    return far ? DVec3{1.0e7, 0.0, -1.0e7} : DVec3{0, 0, 0};
}

// Ohne GPU: Terrain erzeugen, frames Änderungen anwenden und jeweils neu meshen, Zeiten messen.
export int run_voxel_demo_headless(u64 frames) {
    log::info("Voxel-Demo ohne GPU: {} Änderungen", frames);
    benchmark_chunks();
    VoxelScene scene;
    for (u64 i = 0; i < frames; ++i) {
        scene.scripted_edit(i);
        scene.remesh_if_needed();
        profile::frame_mark();
    }
    log::info("Voxel-Terrain: {}", scene.stats());
    if (scene.data().meshes.empty()) {
        log::error("Das Terrain hat kein Netz");
        return 1;
    }
    return 0;
}

// Offscreen auf der GPU zeichnen, ohne Fenster: Szene ändern, zeichnen und als Bild speichern.
export int run_voxel_demo_offscreen(u64 frames, const std::string& image_path, bool baseline,
                                    bool far) {
    auto device = rhi::vulkan::create_device({
        .application_name = "NGine Voxel-Demo",
#ifdef NDEBUG
        .enable_validation = false,
#else
        .enable_validation = true,
#endif
        .disabled_features = {.mesh_shader = baseline, .ray_tracing = baseline},
    });
    if (!device) {
        log::fatal("{}", device.error());
        return 1;
    }

    int exit_code = 0;
    {
        constexpr rhi::Extent2D extent{960, 540};
        auto target =
            (*device)->create_texture({.extent = extent, .format = rhi::Format::RGBA8Srgb});
        if (!target) {
            log::fatal("{}", target.error());
            return 1;
        }
        VoxelScene scene{voxel_origin(far)};
        if (auto result = scene.create_gpu(**device, rhi::Format::RGBA8Srgb); !result) {
            log::fatal("{}", result.error());
            return 1;
        }
        render::TexturePool pool{**device};
        // Ein Bild mehr als Änderungen, damit auch ohne Änderungen etwas zu sehen ist.
        for (u64 i = 0; i <= frames; ++i) {
            if (i < frames) {
                scene.scripted_edit(i);
            }
            scene.remesh_if_needed();
            scene.update(1.0f / 60.0f, extent);
            render::RenderGraph graph;
            const auto output = graph.import_texture(
                "Bild", **target, rhi::ResourceState::Undefined, rhi::ResourceState::TransferSrc);
            scene.add_passes(**device, graph, output);
            (*device)->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
            pool.end_frame();
        }
        log::info("Voxel-Terrain: {}", scene.stats());
        const auto pixels = (*device)->read_texture(**target, rhi::ResourceState::TransferSrc);
        if (auto written = write_ppm(image_path, pixels, extent); !written) {
            log::error("Bild lässt sich nicht schreiben: {}", written.error());
            exit_code = 1;
        } else {
            log::info("Bild gespeichert: {}", image_path);
        }
        scene.release_gpu();
    }
    if (const auto errors = (*device)->validation_errors(); errors > 0) {
        log::error("Die Validation Layers haben {} Fehler gemeldet", errors);
        exit_code = 1;
    }
    return exit_code;
}

} // namespace sandbox
