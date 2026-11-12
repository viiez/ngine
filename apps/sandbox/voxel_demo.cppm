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
#include <thread>
#include <utility>
#include <vector>

// Voxel-Terrain in der Sandbox. Seit R3.1 ein Terrain aus mehreren Chunks als Komponente der
// Welt, gemeshed im Job-System und gezeichnet vom Renderer mit PBR und Schatten (vorher, im
// Durchstich D1, ein Chunk mit eigenem Pass). Mit der Maus veränderbar, auch über Chunk-Grenzen.
// Seit R3.2 entsteht es aus dem Generator mit Startwert (Hügel, Felswände, Höhlen, Überhänge) und
// zeigt Materialien mit Texturen aus dem Asset-Ordner; ein Flugmodus führt in die Höhlen. Seit R3.3
// ist es endlos: Es lädt sich um die Kamera und die Mitte der Kreisbahn nach, in der Ferne in
// gröberen Stufen, bis rund 2 km weit. Ein Autopilot fliegt einige Kilometer und misst dabei
// Frame-Zeiten und Speicher. Seit R3.4 sind Änderungen Deltas gegenüber der Grundwelt: Ein Krater
// bleibt, wenn sein Chunk entladen wird, und ist auch in den groben Stufen zu sehen.
export module sandbox.voxel_demo;

import app.model_scene;
import ngine.asset;
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

f64 mib(usize bytes) {
    return static_cast<f64>(bytes) / (1024.0 * 1024.0);
}

// Wert beim Anteil q (0 bis 1) einer Messreihe.
f64 percentile(std::vector<f64> values, f64 q) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<usize>(q * static_cast<f64>(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

std::string level_counts(const voxel::StreamStats& s) {
    std::string text;
    for (usize level = 0; level < s.per_level.size(); ++level) {
        if (s.per_level[level] > 0) {
            text += std::format("{}S{}={}", text.empty() ? "" : " ", level, s.per_level[level]);
        }
    }
    return text;
}

} // namespace

// Spanne einer Größe während einer Messung. Nicht im anonymen Namensraum, weil VoxelScene sie als
// Feld hält.
struct Range {
    f64 min = 1e300;
    f64 max = 0.0;
    void add(f64 value) {
        min = std::min(min, value);
        max = std::max(max, value);
    }
};

// Terrain der Demo (seit R3.3): endlos um die Beobachter, Sichtweite view in Metern, 64 m hoch.
export world::Terrain demo_terrain(u32 seed = 1, f32 view = 2000.0f) {
    return world::Terrain{.size_y = 4, .seed = seed, .streaming = true, .view_distance = view};
}

// Zustand der Demo: Welt mit Terrain, Sonne und Kamera, Pinsel, Renderer.
export class VoxelScene {
public:
    // origin: Lage des Terrains in der Welt. Weit vom Ursprung zeigt die Demo, dass nichts
    // zittert, weil relativ zur Kamera gezeichnet wird (--far).
    explicit VoxelScene(DVec3 origin = DVec3{0, 0, 0}, world::Terrain terrain = demo_terrain())
        : types_{make_types()}, world_{types_}, origin_{origin} {
        terrain_ = world_.create("Terrain");
        terrain_.set(world::Transform{.position = origin}).set(terrain);
        world_.create("Sonne")
            .set(world::Transform{.rotation = math::look_rotation(
                                      math::normalize(Vec3{0.5f, -0.7f, -0.45f}), {0, 1, 0})})
            .set(render::DirectionalLight{.color = {1.0f, 0.95f, 0.88f}, .intensity = 3.0f});
        camera_ = world_.create("Kamera");
        // Um die Kamera lädt das Terrain (R3.3).
        camera_.set(world::Transform{})
            .set(render::Camera{.near_plane = 0.2f})
            .set(world::TerrainObserver{});
        // Mitte der Oberfläche: Hier treffen sich vier Chunks, Änderungen kreuzen ihre Grenzen.
        generator_ = voxel::Generator{world::generator_desc(terrain)};
        center_ = {0.0f, static_cast<f32>(generator_.height(0, 0) * voxel::voxel_size), 0.0f};
        distance_ = 96.0f;
        update_camera();
        // Erstes Laden in einem Schritt, damit das erste Bild schon die ganze Sicht zeigt.
        const auto start = std::chrono::steady_clock::now();
        remesh(true);
        log::info("Voxel-Demo: Terrain bis {:.0f} m geladen in {:.0f} ms ({} Chunks: {})",
                  terrain.view_distance, elapsed_ms(start), data().stats.stream.leaves,
                  level_counts(data().stats.stream));
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
    // Flugmodus (R3.2): freie Kamera statt der Kreisbahn, beginnt an der Lage der Kreisbahn.
    void toggle_fly() {
        flying_ = !flying_;
        if (flying_) {
            const Vec3 forward = math::to_mat3(rotation_) * Vec3{0.0f, 0.0f, -1.0f};
            fly_yaw_ = std::atan2(-forward.x, -forward.z);
            fly_pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
        }
        log::info("Voxel-Demo: {}", flying_ ? "Flugmodus" : "Kreisbahn");
    }
    [[nodiscard]] bool flying() const noexcept {
        return flying_;
    }
    // Zeigt die Grenzen der Chunks mit Netz, nach Stufe gefärbt (F1).
    void toggle_chunk_bounds() {
        chunk_bounds_ = !chunk_bounds_;
    }
    // Wartet in jedem Frame, bis das Terrain zu den Beobachtern passt (Bilder offscreen).
    void set_wait_for_terrain(bool wait) {
        wait_ = wait;
    }

    // Autopilot (R3.3): fliegt distance Meter geradeaus in 90 m Höhe mit speed m/s und misst
    // Frame-Zeiten, Übernahmen des Streamings und Speicher. Läuft über update().
    void start_flight(f64 distance, f32 speed) {
        flying_ = true;
        flight_ = Flight{.distance = distance, .speed = speed, .start = eye_};
        flight_->start.y = origin_.y + 90.0;
        eye_ = flight_->start;
        flight_->commits = data().stats.stream.commits;
        flight_->generated = data().stats.stream.generated_total;
        if (speed > 0.0f) {
            log::info("Voxel-Demo: Autopilot fliegt {:.1f} km mit {:.0f} m/s", distance / 1000.0,
                      speed);
        } else {
            log::info("Voxel-Demo: Autopilot schwebt {:.0f} s", distance);
        }
    }
    [[nodiscard]] bool flight_active() const noexcept {
        return flight_.has_value();
    }
    // Ergebnis des letzten Flugs, leer, solange keiner beendet ist.
    [[nodiscard]] const std::string& flight_report() const noexcept {
        return flight_report_;
    }

    // --- Ablauf -------------------------------------------------------------------------------

    // Bewegt die Kamera (Kreisbahn oder im Flugmodus nach input) und wendet den gehaltenen
    // Pinsel an (30-mal pro Sekunde).
    void update(f32 dt, rhi::Extent2D viewport, const app::FlyInput& input = {}) {
        if (flight_) {
            autopilot(dt);
        } else if (flying_) {
            fly(dt, input);
        } else {
            if (rotating_) {
                yaw_ += dt * 0.15f;
            }
            update_camera();
        }
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
        // Über alle Stufen: Auch ein ferner Hang lässt sich treffen.
        if (const auto hit = world::raycast_terrain(terrain_, eye_, direction, 1000.0)) {
            edit({.mode = *brush_,
                  .center = math::to_f32(world::terrain_voxels(terrain_, *hit)),
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
        const auto hit =
            world::raycast_terrain(terrain_, world::terrain_world(terrain_, math::to_f64(above)),
                                   {0.0f, -1.0f, 0.0f}, 400.0);
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
              .center = math::to_f32(world::terrain_voxels(terrain_, *hit)),
              .radius = 4.0f,
              .material = 2,
              .strength = 0.8f});
    }

    // Als Delta gegenüber der Grundwelt (R3.4): bleibt beim Entladen erhalten und erscheint in
    // den groben Stufen.
    void edit(const voxel::Brush& brush) {
        static_cast<void>(world::edit_terrain(terrain_, brush));
        ++edits_;
    }

    // Ein Schritt des Terrains, einmal pro Frame: Streaming um die Beobachter (Kamera und, auf
    // der Kreisbahn, ihre Mitte), neue Netze im Job-System. Liefert die Zeit in Millisekunden.
    f64 remesh(bool wait = false) {
        NGINE_PROFILE_ZONE("VoxelScene::remesh");
        const auto start = std::chrono::steady_clock::now();
        // Weltlage der Kamera für ihren TerrainObserver.
        static_cast<void>(world::update_transforms(world_));
        const std::array center{origin_ + math::to_f64(center_)};
        const std::span<const DVec3> extra =
            flying_ ? std::span<const DVec3>{} : std::span<const DVec3>{center};
        const usize meshed =
            world::update_terrains(world_, jobs_, {.observers = extra, .wait = wait || wait_});
        const f64 ms = elapsed_ms(start);
        if (flight_) {
            flight_->update_ms.push_back(ms);
        }
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
        const voxel::StreamStats& st = s.stream;
        std::string text = std::format(
            "{} Chunks ({}; {} einheitlich, {:.1f} MiB Samples), {} Netze mit {} Dreiecken "
            "({:.1f} MiB), Streaming: {} Übernahmen (zuletzt {:.1f} ms, höchstens {:.1f} ms), "
            "{} in Arbeit, {} Chunks erzeugt ({:.2f} ms pro Chunk in Jobs); {} Änderungen "
            "(Deltas: {} Chunks, {:.1f} KiB; {} grobe Blätter neu erzeugt); "
            "Netze im Mittel {:.2f} ms, höchstens {:.2f} ms ({} Läufe, {} Netze)",
            s.chunks, level_counts(st), s.uniform_chunks, mib(s.sample_bytes), s.meshes,
            s.triangles, mib(s.mesh_bytes), st.commits, st.last_commit_ms, st.max_commit_ms,
            st.queued + st.generating + st.generated, st.generated_total,
            st.generate_ms_total / static_cast<f64>(std::max<u64>(st.generated_total, 1)), edits_,
            s.delta_chunks, static_cast<f64>(s.delta_bytes) / 1024.0, st.refreshed_total,
            total_ms_ / static_cast<f64>(std::max<u64>(mesh_count_, 1)), max_ms_, mesh_count_,
            meshed_chunks_);
        if (renderer_) {
            const render::RenderStats& r = renderer_->stats();
            text +=
                std::format("; {} Draws ({} Chunks, {} Meshlets), {} Schatten-Draws, {:.1f} MiB "
                            "Geometrie auf der GPU; GPU ({}): {} Meshlets gezeichnet, "
                            "aussortiert {} Stumpf, {} Rückseiten, {} verdeckt",
                            r.draws, r.terrain_chunks, r.meshlets, r.shadow_draws,
                            mib(renderer_->geometry().stats().used),
                            r.mesh_shaders ? "Mesh Shader" : "indirekt", r.meshlets_drawn,
                            r.culled_frustum, r.culled_backface, r.culled_occluded);
        }
        return text;
    }

    // --- GPU ----------------------------------------------------------------------------------

    // database: Asset-Ordner mit den Texturen der Materialien (nullptr: nur Farben).
    rhi::Result<void> create_gpu(rhi::Device& device, rhi::Format color_format,
                                 const asset::AssetDatabase* database = nullptr) {
        auto renderer = render::Renderer::create(device, color_format);
        if (!renderer) {
            return std::unexpected(renderer.error());
        }
        renderer_ = *std::move(renderer);
        // Schatten über das ganze Terrain.
        renderer_->shadows.distance = 160.0f;
        resources_ = std::make_unique<asset::ResourceManager>(database, &jobs_);
        render::register_loaders(*resources_, *renderer_);
        renderer_->set_resources(resources_.get());
        renderer_->set_terrain_materials(app::terrain_materials(database));
        return {};
    }

    void release_gpu() {
        // Texturen geben ihren Platz im Renderer frei, also vor ihm.
        if (renderer_) {
            renderer_->set_resources(nullptr);
        }
        resources_.reset();
        renderer_.reset();
    }

    // Wartet, bis alle angefragten Texturen gelesen sind (Bilder offscreen).
    void wait_for_assets() {
        if (resources_) {
            resources_->wait();
        }
    }

    // Fügt dem Graph die Pässe des Renderers hinzu, die die Welt in target zeichnen.
    void add_passes(rhi::Device& /*device*/, render::RenderGraph& graph,
                    render::TextureHandle target) {
        NGINE_PROFILE_ZONE("VoxelScene::add_passes");
        const auto start = std::chrono::steady_clock::now();
        static_cast<void>(world::update_transforms(world_));
        resources_->update();
        if (chunk_bounds_) {
            draw_chunk_bounds();
        }
        renderer_->add_passes(graph, world_, target);
        // Beim Flug: Zeit im Hauptthread für die Pässe, darin das Hochladen neuer Netze.
        if (flight_) {
            flight_->passes_ms.push_back(elapsed_ms(start));
            flight_->uploads = std::max(flight_->uploads, renderer_->stats().terrain_uploads);
        }
    }

private:
    void update_camera() {
        const f32 pitch = 0.5f;
        const Vec3 offset{std::cos(yaw_) * std::cos(pitch), std::sin(pitch),
                          std::sin(yaw_) * std::cos(pitch)};
        const DVec3 target = origin_ + math::to_f64(center_);
        eye_ = target + math::to_f64(offset * distance_);
        rotation_ = math::look_rotation(math::to_f32(target - eye_), Vec3{0, 1, 0});
        camera_.set(world::Transform{.position = eye_, .rotation = rotation_});
    }

    void fly(f32 dt, const app::FlyInput& input) {
        fly_yaw_ -= input.look_x * 0.0025f;
        fly_pitch_ = std::clamp(fly_pitch_ - input.look_y * 0.0025f, -1.5f, 1.5f);
        rotation_ = math::angle_axis(fly_yaw_, {0.0f, 1.0f, 0.0f}) *
                    math::angle_axis(fly_pitch_, {1.0f, 0.0f, 0.0f});
        const Vec3 forward = rotation_ * Vec3{0.0f, 0.0f, -1.0f};
        const Vec3 right = rotation_ * Vec3{1.0f, 0.0f, 0.0f};
        Vec3 move = forward * (static_cast<f32>(input.forward) - static_cast<f32>(input.back)) +
                    right * (static_cast<f32>(input.right) - static_cast<f32>(input.left));
        move.y += static_cast<f32>(input.up) - static_cast<f32>(input.down);
        const f32 speed = input.fast ? 40.0f : 10.0f;
        eye_ = eye_ + math::to_f64(math::normalize(move) * (speed * dt));
        camera_.set(world::Transform{.position = eye_, .rotation = rotation_});
    }

    void autopilot(f32 dt) {
        Flight& f = *flight_;
        // Die ersten Frames laufen noch mit dem Wechsel in den Flug; sie zählen nicht.
        if (++f.frames > 10) {
            f.frame_ms.push_back(static_cast<f64>(dt) * 1000.0);
        }
        f.travelled = std::min(f.distance, f.travelled + static_cast<f64>(f.speed * dt));
        const Vec3 heading = math::normalize(Vec3{1.0f, 0.0f, 0.35f});
        eye_ = f.start + math::to_f64(heading) * f.travelled;
        rotation_ =
            math::look_rotation(math::normalize(heading + Vec3{0.0f, -0.25f, 0.0f}), Vec3{0, 1, 0});
        camera_.set(world::Transform{.position = eye_, .rotation = rotation_});
        const world::TerrainStats& s = data().stats;
        f.samples.add(mib(s.sample_bytes));
        f.meshes.add(mib(s.mesh_bytes));
        f.chunks.add(static_cast<f64>(s.chunks));
        if (renderer_) {
            f.gpu.add(mib(renderer_->geometry().stats().used));
        }
        f.seconds += static_cast<f64>(dt);
        // Mit Geschwindigkeit 0 schwebt die Kamera distance Sekunden lang (Vergleich ohne Flug).
        if (f.speed > 0.0f ? f.travelled < f.distance : f.seconds < f.distance) {
            return;
        }
        const voxel::StreamStats& st = s.stream;
        flight_report_ = std::format(
            "{}: {:.1f} km in {:.1f} s ({:.0f} m/s), {} Frames; Frame-Zeit Median {:.2f} ms, "
            "99. Perzentil {:.2f} ms, höchstens {:.2f} ms; {} Übernahmen (höchstens {:.1f} ms), {} "
            "Chunks erzeugt; Chunks {:.0f} bis {:.0f}, Samples {:.1f} bis {:.1f} MiB, Netze {:.1f} "
            "bis {:.1f} MiB, Geometrie auf der GPU {:.1f} bis {:.1f} MiB; update_terrains() 99. "
            "Perzentil {:.2f} ms, höchstens {:.2f} ms; Pässe 99. Perzentil {:.2f} ms, höchstens "
            "{:.2f} ms, bis {} Netze in einem Frame hochgeladen",
            f.speed > 0.0f ? "Flug" : "Schweben", f.travelled / 1000.0, f.seconds,
            f.travelled / std::max(f.seconds, 1e-3), f.frame_ms.size(), percentile(f.frame_ms, 0.5),
            percentile(f.frame_ms, 0.99), percentile(f.frame_ms, 1.0), st.commits - f.commits,
            st.max_commit_ms, st.generated_total - f.generated, f.chunks.min, f.chunks.max,
            f.samples.min, f.samples.max, f.meshes.min, f.meshes.max, renderer_ ? f.gpu.min : 0.0,
            renderer_ ? f.gpu.max : 0.0, percentile(f.update_ms, 0.99),
            percentile(f.update_ms, 1.0), percentile(f.passes_ms, 0.99),
            percentile(f.passes_ms, 1.0), f.uploads);
        log::info("{}", flight_report_);
        flight_.reset();
        const Vec3 forward = math::to_mat3(rotation_) * Vec3{0.0f, 0.0f, -1.0f};
        fly_yaw_ = std::atan2(-forward.x, -forward.z);
        fly_pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    }

    void draw_chunk_bounds() {
        render::DebugDraw& debug = renderer_->debug();
        const DVec3 base = terrain_.get<world::WorldTransform>()->position();
        // Farbe nach Stufe: fein gelb, dann orange, rot, violett, blau, türkis.
        constexpr std::array<Vec3, 6> colors{{{0.95f, 0.85f, 0.2f},
                                              {0.95f, 0.55f, 0.15f},
                                              {0.9f, 0.2f, 0.15f},
                                              {0.7f, 0.2f, 0.8f},
                                              {0.25f, 0.4f, 0.95f},
                                              {0.2f, 0.85f, 0.85f}}};
        for (const auto& [coord, mesh] : data().meshes) {
            const DVec3 corner = base + voxel::chunk_position(coord);
            const f64 size = voxel::chunk_size * static_cast<f64>(1u << coord.level);
            debug.aabb(corner, corner + DVec3{size, size, size},
                       colors[std::min<usize>(coord.level, colors.size() - 1)]);
        }
    }

    struct Flight {
        f64 distance = 0.0;
        f32 speed = 0.0f;
        DVec3 start;
        f64 travelled = 0.0;
        f64 seconds = 0.0;
        u64 frames = 0;
        std::vector<f64> frame_ms;
        u64 commits = 0;
        u64 generated = 0;
        Range samples, meshes, chunks, gpu;
        // Zeit von update_terrains() und der Pässe des Renderers pro Frame, meiste Uploads in
        // einem Frame.
        std::vector<f64> update_ms;
        std::vector<f64> passes_ms;
        u32 uploads = 0;
    };

    // Das Job-System lebt länger als die Welt: Der Streamer des Terrains wartet beim Zerstören auf
    // seine Jobs.
    jobs::JobSystem jobs_;
    reflect::TypeRegistry types_;
    world::World world_;
    voxel::Generator generator_;
    std::unique_ptr<render::Renderer> renderer_;
    // Nach dem Renderer, damit er zuerst stirbt: Texturen geben ihren Platz im Renderer frei.
    std::unique_ptr<asset::ResourceManager> resources_;
    world::Entity terrain_;
    world::Entity camera_;

    DVec3 origin_;
    // Mitte der Oberfläche in Metern relativ zu origin_.
    Vec3 center_;
    f32 distance_ = 64.0f;
    f32 yaw_ = 0.6f;
    bool rotating_ = true;
    bool flying_ = false;
    f32 fly_yaw_ = 0.0f;
    f32 fly_pitch_ = 0.0f;
    bool chunk_bounds_ = false;
    bool wait_ = false;
    std::optional<Flight> flight_;
    std::string flight_report_;
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

// Misst Erzeugen (Generator, R3.2) und Meshing vieler Chunks auf einem Thread und parallel im
// Job-System.
void benchmark_chunks(u32 seed) {
    const world::Terrain terrain{.size_x = 8, .size_y = 4, .size_z = 8, .seed = seed};
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
        log::info("Chunks {} ({} Threads): {} Chunks erzeugt in {:.1f} ms ({:.3f} ms pro Chunk, "
                  "Prüfsumme {:016x}); {} Netze gemeshed in {:.1f} ms ({:.3f} ms pro Netz), {} "
                  "Dreiecke, {} KiB Netze, {} KiB Samples ({} von {} Chunks einheitlich)",
                  parallel ? "parallel" : "seriell", jobs.worker_count() + 1, chunks.size(),
                  fill_ms, fill_ms / static_cast<f64>(std::max<usize>(chunks.size(), 1)),
                  voxel::checksum(chunks), coords.size(), mesh_ms,
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

namespace {

// Streaming ohne GPU um mehrere Beobachter (R3.3), wie auf einem dedizierten Server mit Spielern:
// erstes Laden, dann frames Frames zu 60 Hz, in denen drei Beobachter in verschiedene Richtungen
// fliegen. Misst die Zeit von update_terrains() pro Frame und den Speicher.
void benchmark_streaming(u32 seed, u64 frames, f32 view) {
    // Das Job-System lebt länger als die Welt (der Streamer wartet beim Zerstören auf Jobs).
    jobs::JobSystem jobs;
    const reflect::TypeRegistry types = make_types();
    world::World world{types};
    world::Entity terrain = world.create("Terrain").set(demo_terrain(seed, view));
    struct Player {
        world::Entity entity;
        DVec3 position;
        DVec3 velocity;
    };
    constexpr f64 speed = 80.0;
    std::array<Player, 3> players{{
        {{}, {0.0, 90.0, 0.0}, {speed, 0.0, 0.0}},
        {{}, {3000.0, 90.0, -1500.0}, {0.0, 0.0, speed}},
        {{}, {-2500.0, 90.0, 2000.0}, {-speed * 0.6, 0.0, -speed * 0.8}},
    }};
    for (usize i = 0; i < players.size(); ++i) {
        players[i].entity = world.create(std::format("Spieler {}", i + 1))
                                .set(world::Transform{.position = players[i].position})
                                .set(world::TerrainObserver{});
    }
    static_cast<void>(world::update_transforms(world));
    auto start = std::chrono::steady_clock::now();
    world::update_terrains(world, jobs, {.wait = true});
    const world::TerrainData& data = *terrain.get<world::TerrainState>()->data;
    log::info("Streaming um {} Beobachter ({} Threads): bis {:.0f} m geladen in {:.0f} ms, {} "
              "Chunks ({}), {:.1f} MiB Samples, {} Netze mit {} Dreiecken ({:.1f} MiB)",
              players.size(), jobs.worker_count() + 1, view, elapsed_ms(start), data.stats.chunks,
              level_counts(data.stats.stream), mib(data.stats.sample_bytes), data.stats.meshes,
              data.stats.triangles, mib(data.stats.mesh_bytes));

    std::vector<f64> update_ms;
    Range samples;
    Range chunks;
    const u64 commits = data.stats.stream.commits;
    const u64 generated = data.stats.stream.generated_total;
    constexpr auto frame = std::chrono::microseconds{16667};
    auto next = std::chrono::steady_clock::now();
    for (u64 i = 0; i < frames; ++i) {
        for (Player& p : players) {
            p.position = p.position + p.velocity * (1.0 / 60.0);
            p.entity.set(world::Transform{.position = p.position});
        }
        static_cast<void>(world::update_transforms(world));
        start = std::chrono::steady_clock::now();
        world::update_terrains(world, jobs);
        update_ms.push_back(elapsed_ms(start));
        samples.add(mib(data.stats.sample_bytes));
        chunks.add(static_cast<f64>(data.stats.chunks));
        profile::frame_mark();
        next += frame;
        std::this_thread::sleep_until(next);
    }
    if (frames > 0) {
        log::info("Streaming über {} Frames ({:.0f} m pro Beobachter): update_terrains() Median "
                  "{:.2f} ms, 99. Perzentil {:.2f} ms, höchstens {:.2f} ms; {} Übernahmen "
                  "(höchstens {:.1f} ms), {} Chunks erzeugt ({:.2f} ms pro Chunk in Jobs); Chunks "
                  "{:.0f} bis {:.0f}, Samples {:.1f} bis {:.1f} MiB",
                  frames, speed * static_cast<f64>(frames) / 60.0, percentile(update_ms, 0.5),
                  percentile(update_ms, 0.99), percentile(update_ms, 1.0),
                  data.stats.stream.commits - commits, data.stats.stream.max_commit_ms,
                  data.stats.stream.generated_total - generated,
                  data.stats.stream.generate_ms_total /
                      static_cast<f64>(std::max<u64>(data.stats.stream.generated_total, 1)),
                  chunks.min, chunks.max, samples.min, samples.max);
    }
}

} // namespace

// Ohne GPU: Erzeugen und Meshing messen, Streaming um mehrere Beobachter (R3.3), dann frames
// Änderungen an der Mitte der Demo anwenden und jeweils neu meshen.
export int run_voxel_demo_headless(u64 frames, u32 seed, f32 view) {
    log::info("Voxel-Demo ohne GPU: {} Frames, Startwert {}, Sichtweite {:.0f} m", frames, seed,
              view);
    benchmark_chunks(seed);
    benchmark_streaming(seed, frames, view);
    VoxelScene scene{voxel_origin(false), demo_terrain(seed, view)};
    for (u64 i = 0; i < frames; ++i) {
        scene.scripted_edit(i);
        scene.remesh();
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
                                    bool far, u32 seed, f32 view,
                                    const asset::AssetDatabase* database) {
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
        VoxelScene scene{voxel_origin(far), demo_terrain(seed, view)};
        // Jedes Bild mit fertig geladenem Terrain, damit es nicht vom Tempo der Jobs abhängt.
        scene.set_wait_for_terrain(true);
        if (auto result = scene.create_gpu(**device, rhi::Format::RGBA8Srgb, database); !result) {
            log::fatal("{}", result.error());
            return 1;
        }
        render::TexturePool pool{**device};
        // Ein Bild mehr als Änderungen, damit auch ohne Änderungen etwas zu sehen ist.
        for (u64 i = 0; i <= frames; ++i) {
            if (i < frames) {
                scene.scripted_edit(i);
            }
            scene.remesh();
            scene.update(1.0f / 60.0f, extent);
            render::RenderGraph graph;
            const auto output = graph.import_texture(
                "Bild", **target, rhi::ResourceState::Undefined, rhi::ResourceState::TransferSrc);
            scene.add_passes(**device, graph, output);
            (*device)->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
            pool.end_frame();
            // Der erste Frame fragt die Texturen an; danach mit ihnen weiter.
            if (i == 0) {
                scene.wait_for_assets();
            }
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
