module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
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

// Durchstich D1: ein Chunk Voxel-Terrain mit einem glatten Hügel, als Netz aus Surface Nets
// gezeichnet und zur Laufzeit veränderbar. Wegwerf-Code: Kamera, Matrizen und das Hochladen der
// Netze wandern in R1 in den Renderer und die Mathematik-Bibliothek.
export module sandbox.voxel_demo;

import ngine.core;
import ngine.filesystem;
import ngine.profile;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.shaders;
import ngine.voxel;

namespace sandbox {

using namespace ngine;
using voxel::Vec3;

namespace {

constexpr f32 pi = 3.14159265359f;

// Matrix in Zeilen, so wie voxel.slang sie erwartet.
using Mat4 = std::array<f32, 16>;

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (usize row = 0; row < 4; ++row) {
        for (usize col = 0; col < 4; ++col) {
            f32 sum = 0.0f;
            for (usize k = 0; k < 4; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + col];
            }
            r[row * 4 + col] = sum;
        }
    }
    return r;
}

// Rechtshändig, y oben, Blick entlang -z, Tiefe 0 (nah) bis 1 (fern).
Mat4 perspective(f32 fov_y, f32 aspect, f32 z_near, f32 z_far) {
    const f32 sy = 1.0f / std::tan(fov_y * 0.5f);
    const f32 sx = sy / aspect;
    return {sx,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            sy,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            z_far / (z_near - z_far),
            z_near * z_far / (z_near - z_far),
            0.0f,
            0.0f,
            -1.0f,
            0.0f};
}

struct Camera {
    Vec3 eye;
    Vec3 forward;
    Vec3 right;
    Vec3 up;
    f32 fov_y = 50.0f * pi / 180.0f;

    [[nodiscard]] Mat4 view() const {
        return {right.x,    right.y,    right.z,    -voxel::dot(right, eye),
                up.x,       up.y,       up.z,       -voxel::dot(up, eye),
                -forward.x, -forward.y, -forward.z, voxel::dot(forward, eye),
                0.0f,       0.0f,       0.0f,       1.0f};
    }

    // Strahl durch einen Bildpunkt (Ursprung oben links).
    [[nodiscard]] Vec3 ray(f32 x, f32 y, rhi::Extent2D extent) const {
        const f32 aspect = static_cast<f32>(extent.width) / static_cast<f32>(extent.height);
        const f32 t = std::tan(fov_y * 0.5f);
        const f32 nx = 2.0f * x / static_cast<f32>(extent.width) - 1.0f;
        const f32 ny = 1.0f - 2.0f * y / static_cast<f32>(extent.height);
        return voxel::normalize(forward + right * (nx * t * aspect) + up * (ny * t));
    }
};

Camera look_at(Vec3 eye, Vec3 target) {
    Camera c;
    c.eye = eye;
    c.forward = voxel::normalize(target - eye);
    c.right = voxel::normalize(voxel::cross(c.forward, Vec3{0.0f, 1.0f, 0.0f}));
    c.up = voxel::cross(c.right, c.forward);
    return c;
}

struct PushConstants {
    Mat4 view_projection;
    std::array<f32, 4> light;
    std::array<f32, 4> camera;
    u64 vertices;
};
static_assert(sizeof(PushConstants) <= rhi::max_push_constant_size);

// Netz auf der GPU, eines pro Frame im Flug, damit die CPU nie in einen Puffer schreibt, den die
// GPU gerade liest.
struct GpuMesh {
    std::unique_ptr<rhi::Buffer> vertices;
    std::unique_ptr<rhi::Buffer> indices;
    u32 index_count = 0;
    u64 version = ~u64{0};
};

// Puffer mindestens der Größe size, mit Luft nach oben, damit nicht jede Änderung neu anfordert.
bool ensure_capacity(rhi::Device& device, std::unique_ptr<rhi::Buffer>& buffer, u64 size,
                     rhi::BufferUsage usage) {
    if (buffer && buffer->size() >= size) {
        return true;
    }
    auto created =
        device.create_buffer({.size = std::max<u64>(size + size / 2, 4096), .usage = usage});
    if (!created) {
        log::error("Puffer für das Voxel-Netz: {}", created.error());
        return false;
    }
    buffer = *std::move(created);
    return true;
}

} // namespace

// Zustand der Demo: Gitter, Netz, Kamera, Pinsel und GPU-Ressourcen.
export class VoxelScene {
public:
    explicit VoxelScene(u32 voxels = 32) : grid_{{voxels + 1, voxels + 1, voxels + 1}} {
        voxel::fill_hill(
            grid_, {.ground = 6.0f, .height = 14.0f, .radius = static_cast<f32>(voxels) * 0.28f});
        center_ = {static_cast<f32>(voxels) * 0.5f, 10.0f, static_cast<f32>(voxels) * 0.5f};
        distance_ = static_cast<f32>(voxels) * 1.5f;
        camera_ = make_camera();
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
        distance_ = std::clamp(distance_ * std::pow(0.9f, steps), 10.0f, 200.0f);
    }
    void toggle_rotation() {
        rotating_ = !rotating_;
    }

    // --- Ablauf -------------------------------------------------------------------------------

    // Bewegt die Kamera und wendet den gehaltenen Pinsel an (30-mal pro Sekunde).
    void update(f32 dt, rhi::Extent2D viewport) {
        if (rotating_) {
            yaw_ += dt * 0.25f;
        }
        camera_ = make_camera();
        if (!brush_ || !cursor_) {
            return;
        }
        brush_timer_ -= dt;
        if (brush_timer_ > 0.0f) {
            return;
        }
        brush_timer_ = 1.0f / 30.0f;
        const Vec3 direction = camera_.ray(cursor_->first, cursor_->second, viewport);
        if (const auto hit = voxel::raycast(grid_, camera_.eye, direction, 500.0f)) {
            edit({.mode = *brush_,
                  .center = hit->position,
                  .radius = 2.5f,
                  .material = 2,
                  .strength = 0.6f});
        }
    }

    // Automatische Änderung für Tests und die CI: Abtragen auf einem Kreis um die Kuppe, jeder
    // fünfte Schritt trägt wieder auf, jeder siebte glättet.
    void scripted_edit(u64 step) {
        const f32 angle = static_cast<f32>(step) * 0.7f;
        const f32 radius = 3.0f + static_cast<f32>(step % 5);
        const Vec3 above{center_.x + std::cos(angle) * radius, 100.0f,
                         center_.z + std::sin(angle) * radius};
        const auto hit = voxel::raycast(grid_, above, {0.0f, -1.0f, 0.0f}, 200.0f);
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
              .radius = 2.5f,
              .material = 2,
              .strength = 0.8f});
    }

    void edit(const voxel::Brush& brush) {
        voxel::apply(grid_, brush);
        ++edits_;
    }

    // Baut das Netz neu, wenn sich das Gitter geändert hat. Liefert die Zeit in Millisekunden.
    std::optional<f64> remesh_if_needed() {
        if (meshed_version_ == grid_.version()) {
            return std::nullopt;
        }
        return remesh();
    }

    f64 remesh() {
        NGINE_PROFILE_ZONE("VoxelScene::remesh");
        const auto start = std::chrono::steady_clock::now();
        mesher_.build(grid_, mesh_);
        const f64 ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        meshed_version_ = grid_.version();
        ++mesh_count_;
        total_ms_ += ms;
        max_ms_ = std::max(max_ms_, ms);
        return ms;
    }

    [[nodiscard]] const voxel::Mesh& mesh() const {
        return mesh_;
    }
    [[nodiscard]] std::string stats() const {
        return std::format("{} Dreiecke, {} Eckpunkte, {} Änderungen; Meshing im Mittel {:.2f} ms, "
                           "höchstens {:.2f} ms ({} Läufe)",
                           mesh_.triangle_count(), mesh_.vertices.size(), edits_,
                           total_ms_ / static_cast<f64>(std::max<u64>(mesh_count_, 1)), max_ms_,
                           mesh_count_);
    }

    // --- GPU ----------------------------------------------------------------------------------

    rhi::Result<void> create_gpu(rhi::Device& device, rhi::Format color_format) {
        auto vertex = device.create_shader(shaders::voxel_vertex());
        auto fragment = device.create_shader(shaders::voxel_fragment());
        if (!vertex || !fragment) {
            return std::unexpected(!vertex ? vertex.error() : fragment.error());
        }
        auto pipeline = device.create_pipeline({
            .vertex = vertex->get(),
            .fragment = fragment->get(),
            .color_format = color_format,
            .depth_format = rhi::Format::D32Float,
            .cull_mode = rhi::CullMode::Back,
        });
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        vertex_shader_ = *std::move(vertex);
        fragment_shader_ = *std::move(fragment);
        pipeline_ = *std::move(pipeline);
        gpu_meshes_.resize(device.frames_in_flight());
        return {};
    }

    void release_gpu() {
        gpu_meshes_.clear();
        depth_.reset();
        pipeline_.reset();
        fragment_shader_.reset();
        vertex_shader_.reset();
    }

    // Zeichnet die Szene in color. slot ist der Platz des Frames im Flug (rhi::Frame::index).
    void record(rhi::Device& device, rhi::CommandList& commands, rhi::Texture& color, u32 slot) {
        NGINE_PROFILE_ZONE("VoxelScene::record");
        const rhi::Extent2D extent = color.extent();
        if (!depth_ || depth_->extent().width != extent.width ||
            depth_->extent().height != extent.height) {
            // Der alte Tiefenpuffer kann noch in einem Frame im Flug stecken.
            device.wait_idle();
            auto depth = device.create_texture({.extent = extent, .format = rhi::Format::D32Float});
            verify(depth.has_value(), "Tiefenpuffer lässt sich nicht erzeugen");
            depth_ = *std::move(depth);
        }

        GpuMesh& gpu = gpu_meshes_[slot];
        if (gpu.version != meshed_version_) {
            upload(device, gpu);
        }

        const f32 aspect =
            static_cast<f32>(extent.width) / static_cast<f32>(std::max(extent.height, 1u));
        const PushConstants push{
            .view_projection =
                multiply(perspective(camera_.fov_y, aspect, 0.5f, 1000.0f), camera_.view()),
            .light = {0.48f, 0.78f, 0.40f, 0.35f},
            .camera = {camera_.eye.x, camera_.eye.y, camera_.eye.z, 0.003f},
            .vertices = gpu.vertices ? gpu.vertices->device_address() : 0,
        };

        commands.begin_rendering({.color = &color,
                                  .clear_color = rhi::Color{0.45f, 0.60f, 0.80f, 1.0f},
                                  .depth = depth_.get(),
                                  .clear_depth = 1.0f});
        if (gpu.index_count > 0) {
            commands.bind_pipeline(*pipeline_);
            commands.push_constants(std::as_bytes(std::span{&push, 1}));
            commands.bind_index_buffer(*gpu.indices);
            commands.draw_indexed(gpu.index_count);
        }
        commands.end_rendering();
    }

private:
    [[nodiscard]] Camera make_camera() const {
        const f32 pitch = 0.55f;
        const Vec3 offset{std::cos(yaw_) * std::cos(pitch), std::sin(pitch),
                          std::sin(yaw_) * std::cos(pitch)};
        return look_at(center_ + offset * distance_, center_);
    }

    void upload(rhi::Device& device, GpuMesh& gpu) {
        NGINE_PROFILE_ZONE("VoxelScene::upload");
        gpu.index_count = 0;
        gpu.version = meshed_version_;
        if (mesh_.indices.empty()) {
            return;
        }
        const auto vertices = std::as_bytes(std::span{mesh_.vertices});
        const auto indices = std::as_bytes(std::span{mesh_.indices});
        if (!ensure_capacity(device, gpu.vertices, vertices.size(), rhi::BufferUsage::Storage) ||
            !ensure_capacity(device, gpu.indices, indices.size(),
                             rhi::BufferUsage::Storage | rhi::BufferUsage::Index)) {
            return;
        }
        gpu.vertices->write(0, vertices);
        gpu.indices->write(0, indices);
        gpu.index_count = static_cast<u32>(mesh_.indices.size());
    }

    voxel::Grid grid_;
    voxel::Mesher mesher_;
    voxel::Mesh mesh_;
    u64 meshed_version_ = ~u64{0};

    Vec3 center_;
    f32 distance_ = 48.0f;
    f32 yaw_ = 0.6f;
    bool rotating_ = true;
    Camera camera_;

    std::optional<std::pair<f32, f32>> cursor_;
    std::optional<voxel::BrushMode> brush_;
    f32 brush_timer_ = 0.0f;

    u64 edits_ = 0;
    u64 mesh_count_ = 0;
    f64 total_ms_ = 0.0;
    f64 max_ms_ = 0.0;

    std::unique_ptr<rhi::Shader> vertex_shader_;
    std::unique_ptr<rhi::Shader> fragment_shader_;
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::unique_ptr<rhi::Texture> depth_;
    std::vector<GpuMesh> gpu_meshes_;
};

namespace {

// Misst, wie lange Surface Nets für verschieden große Gitter brauchen.
void benchmark_meshing() {
    for (const u32 voxels : {32u, 64u, 128u}) {
        voxel::Grid grid{{voxels + 1, voxels + 1, voxels + 1}};
        voxel::fill_hill(grid, {.ground = static_cast<f32>(voxels) * 0.2f,
                                .height = static_cast<f32>(voxels) * 0.45f,
                                .radius = static_cast<f32>(voxels) * 0.28f});
        voxel::Mesher mesher;
        voxel::Mesh mesh;
        mesher.build(grid, mesh); // Aufwärmen: Speicher anfordern
        constexpr int runs = 5;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < runs; ++i) {
            mesher.build(grid, mesh);
        }
        const f64 ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count() /
            runs;
        log::info(
            "Surface Nets {}³ Voxel: {:.2f} ms, {} Dreiecke, {} KiB Netz, {} KiB Dichtefeld",
            voxels, ms, mesh.triangle_count(),
            (mesh.vertices.size() * sizeof(voxel::Vertex) + mesh.indices.size() * sizeof(u32)) /
                1024,
            grid.samples().size_bytes() / 1024);
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

// Ohne GPU: Hügel erzeugen, frames Änderungen anwenden und jeweils neu meshen, Zeiten messen.
export int run_voxel_demo_headless(u64 frames) {
    log::info("Voxel-Demo ohne GPU: {} Änderungen", frames);
    benchmark_meshing();
    VoxelScene scene;
    for (u64 i = 0; i < frames; ++i) {
        scene.scripted_edit(i);
        scene.remesh_if_needed();
        profile::frame_mark();
    }
    log::info("Voxel-Netz: {}", scene.stats());
    if (scene.mesh().indices.empty()) {
        log::error("Das Voxel-Netz ist leer");
        return 1;
    }
    return 0;
}

// Offscreen auf der GPU zeichnen, ohne Fenster: Szene ändern, zeichnen und als Bild speichern.
export int run_voxel_demo_offscreen(u64 frames, const std::string& image_path, bool baseline) {
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
        VoxelScene scene;
        if (auto result = scene.create_gpu(**device, rhi::Format::RGBA8Srgb); !result) {
            log::fatal("{}", result.error());
            return 1;
        }
        // Ein Bild mehr als Änderungen, damit auch ohne Änderungen etwas zu sehen ist.
        for (u64 i = 0; i <= frames; ++i) {
            if (i < frames) {
                scene.scripted_edit(i);
            }
            scene.remesh_if_needed();
            scene.update(1.0f / 60.0f, extent);
            (*device)->execute(
                [&](rhi::CommandList& commands) { scene.record(**device, commands, **target, 0); });
        }
        log::info("Voxel-Netz: {}", scene.stats());
        const auto pixels = (*device)->read_texture(**target);
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
