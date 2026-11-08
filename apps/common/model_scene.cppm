module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Standardszene von Sandbox und Editor (seit R2.1 hier gemeinsam): ein glTF-Modell oder ein Raster
// aus Kugeln mit steigendem Metallic- und Roughness-Wert, gezeichnet vom Renderer aus der Welt und
// mit einer freien Kamera begehbar. Alles steht auf einem Boden in der Sonne mit Schatten; die
// Standardszene hat dazu eine Reihe Säulen bis in die Ferne, acht kreisende Punktlichter und ein
// Spotlicht.
//
// Seit R2.3 kreisen die Punktlichter über die Komponente Orbit und ein System im Schedule: in der
// Sandbox immer, im Editor nur beim Spielen (simulate()).
//
// Seit R2.4 verweisen MeshRenderer über Asset-Kennungen (ADR 0033). Meshes und Materialien der
// Standardszene sind ein erzeugtes Modell mit fester Kennung (default_scene_id), ein Modell aus
// --model eines mit einer Kennung aus seinem Pfad. Die Szene besitzt den Ressourcen-Manager, aus
// dem der Renderer Modelle lädt; der Editor gibt ihm seine Asset-Datenbank.
export module app.model_scene;

import ngine.core;
import ngine.asset;
import ngine.filesystem;
import ngine.jobs;
import ngine.math;
import ngine.profile;
import ngine.reflect;
import ngine.render;
import ngine.rhi;
import ngine.world;

namespace app {

using namespace ngine;
using math::DVec3;
using math::Vec3;

// Zustand der Eingabe für die freie Kamera, unabhängig von der Plattformschicht.
export struct FlyInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool fast = false;
    // Mausbewegung seit dem letzten update() in Pixeln.
    f32 look_x = 0.0f;
    f32 look_y = 0.0f;
};

// Game: Kamera als Entity der Welt, Lichter kreisen (Sandbox). Editor: Die Kamera gehört nicht zur
// Szene, sondern geht als Renderer::camera_override an den Renderer, und nichts bewegt sich von
// selbst, damit die Szene genau so gespeichert wird, wie sie im Inspector steht (R2.2).
export enum class SceneMode : u8 { Game, Editor };

// Kreisbahn um center mit Auf und Ab, z. B. für die Lichter der Standardszene. Das System
// „Kreisbahnen“ zählt time weiter und setzt daraus Transform; time gehört zum Zustand, damit
// Spielen im Editor und Stoppen ihn mit der Welt sichern und wiederherstellen.
export struct Orbit {
    // Mittelpunkt der Bahn auf der untersten Höhe.
    DVec3 center{0.0, 0.0, 0.0};
    f32 radius_x = 1.0f;
    f32 radius_z = 1.0f;
    // Hub des Auf und Ab in Metern.
    f32 bob = 0.0f;
    // Winkelgeschwindigkeit der Bahn und des Auf und Ab im Bogenmaß pro Sekunde.
    f32 speed = 0.4f;
    f32 bob_speed = 0.9f;
    // Startwinkel.
    f32 phase = 0.0f;
    // Zeit auf der Bahn in Sekunden.
    f32 time = 0.0f;
};

// Ort auf der Bahn zur Zeit orbit.time.
export DVec3 orbit_position(const Orbit& orbit) {
    const f32 angle = orbit.phase + orbit.time * orbit.speed;
    const f32 height =
        orbit.bob * (0.5f + 0.5f * std::sin(orbit.time * orbit.bob_speed + orbit.phase * 2.0f));
    return orbit.center + DVec3{std::cos(angle) * static_cast<f64>(orbit.radius_x), height,
                                std::sin(angle) * static_cast<f64>(orbit.radius_z)};
}

// Registriert die Komponenten der Standardszene (Orbit) in der Reflection.
export void register_types(reflect::TypeRegistry& types) {
    if (types.find<Orbit>() == nullptr) {
        types.add<Orbit>("Orbit")
            .field("center", &Orbit::center)
            .field("radius_x", &Orbit::radius_x)
            .range(0.0, 100.0)
            .field("radius_z", &Orbit::radius_z)
            .range(0.0, 100.0)
            .field("bob", &Orbit::bob)
            .range(0.0, 100.0)
            .field("speed", &Orbit::speed)
            .field("bob_speed", &Orbit::bob_speed)
            .field("phase", &Orbit::phase)
            .hint(reflect::Hint::Angle)
            .field("time", &Orbit::time);
    }
}

namespace {

constexpr u32 grid_size = 7;
constexpr f32 grid_spacing = 2.5f;
constexpr u32 light_count = 8;
// Richtung zur Sonne: links vorn und hoch, Schatten fallen nach rechts hinten.
constexpr Vec3 to_sun{-0.55f, 0.65f, 0.5f};

// Farbe auf dem Farbkreis (Sättigung und Helligkeit voll).
Vec3 hue(f32 h) {
    const auto channel = [&](f32 offset) {
        const f32 k = std::fmod(h * 6.0f + offset, 6.0f);
        return 1.0f - std::max(0.0f, std::min({k, 4.0f - k, 1.0f}));
    };
    return {channel(5.0f), channel(3.0f), channel(1.0f)};
}

math::Quat shine_along(Vec3 direction) {
    const Vec3 d = math::normalize(direction);
    const Vec3 up = std::abs(d.y) > 0.99f ? Vec3{0.0f, 0.0f, -1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    return math::look_rotation(d, up);
}

// Hülle aller MeshRenderer in Weltkoordinaten (relativ zu origin, damit 32 Bit reichen).
std::optional<std::pair<Vec3, Vec3>> world_bounds(world::World& world, render::Renderer& renderer,
                                                  DVec3 origin) {
    constexpr f32 big = std::numeric_limits<f32>::max();
    Vec3 low{big, big, big};
    Vec3 high{-big, -big, -big};
    bool any = false;
    world.each<const world::WorldTransform, const render::MeshRenderer>(
        [&](const world::WorldTransform& transform, const render::MeshRenderer& component) {
            const render::GpuMesh* mesh = renderer.find_mesh(component.mesh);
            if (mesh == nullptr || mesh->submeshes.empty()) {
                return;
            }
            for (u32 corner = 0; corner < 8; ++corner) {
                const Vec3 local{(corner & 1) != 0 ? mesh->bounds_max.x : mesh->bounds_min.x,
                                 (corner & 2) != 0 ? mesh->bounds_max.y : mesh->bounds_min.y,
                                 (corner & 4) != 0 ? mesh->bounds_max.z : mesh->bounds_min.z};
                const Vec3 p =
                    math::to_f32(transform.value.transform_point(math::to_f64(local)) - origin);
                low = math::min(low, p);
                high = math::max(high, p);
                any = true;
            }
        });
    if (!any) {
        return std::nullopt;
    }
    return std::make_pair(low, high);
}

// Meshes der Standardszene im erzeugten Modell (Index in Model::meshes).
enum DefaultMesh : u32 { ground_mesh, sphere_mesh, pillar_mesh, bulb_mesh };
// Materialien: Boden, Stein, acht leuchtende, danach die Kugeln Zeile für Zeile.
enum DefaultMaterial : u32 { ground_material, stone_material, first_glow_material };
constexpr u32 first_sphere_material = first_glow_material + light_count;

} // namespace

// Kennung des erzeugten Modells der Standardszene.
export constexpr asset::AssetId default_scene_id = asset::generated_id("app.model_scene.default");
// Kennung des Bodens unter einem Modell aus --model.
export constexpr asset::AssetId model_ground_id = asset::generated_id("app.model_scene.ground");

export class ModelScene {
public:
    // Ohne model_path das Kugelraster. origin verschiebt alles, auch die Kamera (--far).
    [[nodiscard]] static rhi::Result<std::unique_ptr<ModelScene>>
    create(rhi::Device& device, rhi::Format target_format,
           const std::optional<std::string>& model_path, DVec3 origin,
           SceneMode mode = SceneMode::Game) {
        auto renderer = render::Renderer::create(device, target_format);
        if (!renderer) {
            return std::unexpected(renderer.error());
        }
        auto scene =
            std::unique_ptr<ModelScene>{new ModelScene{*std::move(renderer), origin, mode}};
        scene->add_sun();
        if (model_path) {
            if (auto result = scene->load_model(*model_path); !result) {
                return std::unexpected(result.error());
            }
            scene->frame_camera();
        } else if (auto result = scene->build_default(); !result) {
            return std::unexpected(result.error());
        }
        return scene;
    }

    // F1: Hüllquader, Lichter und Achsenkreuz zeichnen.
    void set_debug_draw(bool enabled) {
        debug_draw_ = enabled;
    }
    void toggle_debug_draw() {
        debug_draw_ = !debug_draw_;
    }
    // F2: Kaskaden der Schatten einfärben.
    void toggle_cascades() {
        renderer_->show_cascades = !renderer_->show_cascades;
    }

    // Bewegt die freie Kamera: W/S vor und zurück, A/D seitwärts, E/Q hoch und runter, Shift
    // schneller; die Maus dreht.
    void update(f32 dt, const FlyInput& input) {
        yaw_ -= input.look_x * 0.0025f;
        pitch_ = std::clamp(pitch_ - input.look_y * 0.0025f, -1.5f, 1.5f);
        const math::Quat rotation = math::angle_axis(yaw_, {0.0f, 1.0f, 0.0f}) *
                                    math::angle_axis(pitch_, {1.0f, 0.0f, 0.0f});
        const Vec3 forward = rotation * Vec3{0.0f, 0.0f, -1.0f};
        const Vec3 right = rotation * Vec3{1.0f, 0.0f, 0.0f};
        Vec3 move{0.0f, 0.0f, 0.0f};
        move = move + forward * (static_cast<f32>(input.forward) - static_cast<f32>(input.back));
        move = move + right * (static_cast<f32>(input.right) - static_cast<f32>(input.left));
        move.y += static_cast<f32>(input.up) - static_cast<f32>(input.down);
        const f32 speed = speed_ * (input.fast ? 4.0f : 1.0f);
        position_ = position_ + math::to_f64(math::normalize(move) * (speed * dt));
        if (mode_ == SceneMode::Game) {
            camera_.set(world::Transform{.position = position_, .rotation = rotation});
            simulate(dt);
        } else {
            renderer_->camera_override = render::CameraView{.position = position_,
                                                            .rotation = math::to_mat3(rotation),
                                                            .camera = camera_settings_};
        }
    }

    // Lässt die Systeme der Szene einen Schritt von dt Sekunden laufen (Lichter kreisen). In der
    // Sandbox ruft update() das in jedem Frame auf, im Editor nur, solange gespielt wird.
    void simulate(f32 dt) {
        NGINE_PROFILE_ZONE("ModelScene::simulate");
        step_ = dt;
        schedule_.run(jobs_);
    }

    void add_passes(render::RenderGraph& graph, render::TextureHandle target) {
        NGINE_PROFILE_ZONE("ModelScene::add_passes");
        resources_->update();
        (void)world::update_transforms(world_);
        if (debug_draw_) {
            draw_debug();
        }
        renderer_->add_passes(graph, world_, target);
    }

    // Welt und Renderer, z. B. für Fenster des Editors.
    [[nodiscard]] world::World& world() noexcept {
        return world_;
    }
    [[nodiscard]] render::Renderer& renderer() noexcept {
        return *renderer_;
    }
    // Ressourcen-Manager der Szene; update() läuft in add_passes().
    [[nodiscard]] asset::ResourceManager& resources() noexcept {
        return *resources_;
    }

    [[nodiscard]] std::string stats() const {
        const render::RenderStats& stats = renderer_->stats();
        return std::format("{} Draws, {} Dreiecke, {} Schatten-Draws, {} Lichter, {} Meshes, {} "
                           "Materialien, {} Debug-Linien",
                           stats.draws, stats.triangles, stats.shadow_draws, stats.lights,
                           renderer_->meshes().count(), renderer_->materials().count(),
                           stats.debug_lines);
    }

private:
    static reflect::TypeRegistry make_types() {
        reflect::TypeRegistry types;
        world::register_types(types);
        render::register_types(types);
        app::register_types(types);
        return types;
    }

    ModelScene(std::unique_ptr<render::Renderer> renderer, DVec3 origin, SceneMode mode)
        : types_{make_types()}, world_{types_}, renderer_{std::move(renderer)},
          jobs_{{.worker_count = 2}},
          resources_{std::make_unique<asset::ResourceManager>(nullptr, &jobs_)}, schedule_{world_},
          origin_{origin}, mode_{mode} {
        renderer_->set_resources(resources_.get());
        render::register_loaders(*resources_, *renderer_);
        schedule_.add<Orbit, world::Transform>("Kreisbahnen",
                                               [this](Orbit& orbit, world::Transform& transform) {
                                                   orbit.time += step_;
                                                   transform.position = orbit_position(orbit);
                                               });
        if (mode_ == SceneMode::Game) {
            camera_ = world_.create("Kamera");
            camera_.set(world::Transform{}).set(render::Camera{});
        }
    }

    // Modell aus einer Datei außerhalb der Asset-Datenbank: sofort laden und unter einer Kennung
    // aus dem Pfad als erzeugtes Modell anmelden.
    rhi::Result<void> load_model(const std::string& path) {
        auto model = asset::load_gltf(path);
        if (!model) {
            return std::unexpected(model.error());
        }
        auto uploaded = render::upload_model(*renderer_, *model);
        if (!uploaded) {
            return std::unexpected(uploaded.error());
        }
        const asset::AssetId id = asset::generated_id("file:" + filesystem::normalize(path));
        static_cast<void>(resources_->add(id, *std::move(uploaded)));
        world::Entity root = render::spawn_model(world_, *model, id, "Modell");
        root.set(world::Transform{.position = origin_});
        log::info("Modell geladen: {} ({} Meshes, {} Materialien, {} Bilder)", path,
                  model->meshes.size(), model->materials.size(), model->images.size());
        return {};
    }

    // Mesh mit einem Teilnetz für ein erzeugtes Modell.
    static asset::Mesh single_mesh(std::string name, asset::Primitive primitive,
                                   std::optional<u32> material = std::nullopt) {
        primitive.material = material;
        asset::Mesh mesh{.name = std::move(name), .primitives = {}};
        mesh.primitives.push_back(std::move(primitive));
        return mesh;
    }

    // Material nach glTF; die Standardwerte von asset::Material (metallisch) passen hier nicht.
    static asset::Material material(std::string name, math::Vec4 color, f32 roughness,
                                    f32 metallic = 0.0f, Vec3 emissive = {0.0f, 0.0f, 0.0f}) {
        asset::Material result;
        result.name = std::move(name);
        result.base_color = color;
        result.metallic = metallic;
        result.roughness = roughness;
        result.emissive = emissive;
        return result;
    }

    // Lädt ein erzeugtes Modell hoch und meldet es unter id an.
    rhi::Result<void> add_generated(asset::AssetId id, const asset::Model& model) {
        auto uploaded = render::upload_model(*renderer_, model);
        if (!uploaded) {
            return std::unexpected(uploaded.error());
        }
        static_cast<void>(resources_->add(id, *std::move(uploaded)));
        return {};
    }

    void add_sun() {
        world_.create("Sonne")
            .set(world::Transform{.rotation = shine_along(-to_sun)})
            .set(render::DirectionalLight{.color = {1.0f, 0.95f, 0.88f}, .intensity = 3.0f});
    }

    static asset::Material ground() {
        return material("Boden", {0.5f, 0.5f, 0.48f, 1.0f}, 0.9f);
    }

    // Boden unter einem Modell aus --model: flacher Quader, Oberseite bei height (relativ zu
    // origin), als eigenes erzeugtes Modell.
    rhi::Result<void> add_ground(f32 half_size, f32 height) {
        asset::Model model;
        model.materials.push_back(ground());
        model.meshes.push_back(
            single_mesh("Boden", asset::make_box({half_size, 0.1f, half_size}), 0));
        if (auto result = add_generated(model_ground_id, model); !result) {
            return result;
        }
        world_.create("Boden")
            .set(world::Transform{.position = origin_ + DVec3{0.0, height - 0.1, 0.0}})
            .set(render::MeshRenderer{.mesh = {.asset = model_ground_id, .index = 0}});
        return {};
    }

    // Erzeugtes Modell der Standardszene: Boden, Kugel, Säule, Leuchtkugel und alle Materialien.
    rhi::Result<void> add_default_model() {
        asset::Model model;
        model.meshes.push_back(
            single_mesh("Boden", asset::make_box({60.0f, 0.1f, 60.0f}), ground_material));
        model.meshes.push_back(single_mesh("Kugel", asset::make_sphere(1.0f, 64, 32)));
        model.meshes.push_back(
            single_mesh("Säule", asset::make_box({0.5f, 3.0f, 0.5f}), stone_material));
        model.meshes.push_back(single_mesh("Leuchtkugel", asset::make_sphere(0.12f, 16, 8)));
        model.materials.push_back(ground());
        model.materials.push_back(material("Stein", {0.75f, 0.72f, 0.66f, 1.0f}, 0.7f));
        for (u32 i = 0; i < light_count; ++i) {
            const Vec3 color = hue(static_cast<f32>(i) / static_cast<f32>(light_count));
            model.materials.push_back(material(std::format("Leuchten {}", i),
                                               {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, 0.0f, color * 4.0f));
        }
        for (u32 row = 0; row < grid_size; ++row) {
            for (u32 column = 0; column < grid_size; ++column) {
                const f32 metallic = static_cast<f32>(row) / static_cast<f32>(grid_size - 1);
                const f32 roughness =
                    std::max(static_cast<f32>(column) / static_cast<f32>(grid_size - 1), 0.05f);
                model.materials.push_back(material(std::format("Kugel {} {}", row, column),
                                                   {0.9f, 0.45f, 0.2f, 1.0f}, roughness, metallic));
            }
        }
        return add_generated(default_scene_id, model);
    }

    static asset::AssetRef default_ref(u32 index) {
        return {.asset = default_scene_id, .index = index};
    }

    // Standardszene: Kugelraster auf dem Boden, Säulen bis in 60 m Entfernung, acht kreisende
    // Punktlichter mit leuchtenden Kugeln und ein Spotlicht.
    rhi::Result<void> build_default() {
        renderer_->shadows.distance = 100.0f;
        if (auto result = add_default_model(); !result) {
            return result;
        }
        world_.create("Boden")
            .set(world::Transform{.position = origin_ + DVec3{0.0, -0.1, 0.0}})
            .set(render::MeshRenderer{.mesh = default_ref(ground_mesh)});
        build_spheres();

        for (u32 i = 0; i < 16; ++i) {
            const f64 side = (i % 2 == 0) ? -12.0 : 12.0;
            const f64 z = -4.0 - static_cast<f64>(i / 2) * 7.0;
            world_.create(std::format("Säule {}", i))
                .set(world::Transform{.position = origin_ + DVec3{side, 3.0, z}})
                .set(render::MeshRenderer{.mesh = default_ref(pillar_mesh)});
        }

        for (u32 i = 0; i < light_count; ++i) {
            const Vec3 color = hue(static_cast<f32>(i) / static_cast<f32>(light_count));
            // Punktlichter kreisen vor dem Kugelraster, jedes auf eigener Höhe.
            const Orbit orbit{.center = origin_ + DVec3{0.0, 0.8, 5.0},
                              .radius_x = 7.0f,
                              .radius_z = 4.0f,
                              .bob = 1.6f,
                              .speed = 0.4f,
                              .bob_speed = 0.9f,
                              .phase = static_cast<f32>(i) / static_cast<f32>(light_count) * 2.0f *
                                       math::pi};
            world_.create(std::format("Licht {}", i))
                .set(world::Transform{.position = orbit_position(orbit)})
                .set(orbit)
                .set(render::PointLight{.color = color, .intensity = 10.0f, .range = 9.0f})
                .set(render::MeshRenderer{.mesh = default_ref(bulb_mesh),
                                          .material = default_ref(first_glow_material + i)});
        }
        world_.create("Spot")
            .set(world::Transform{.position = origin_ + DVec3{0.0, 9.0, 9.0},
                                  .rotation = shine_along({0.0f, -1.0f, -0.35f})})
            .set(render::SpotLight{.color = {1.0f, 0.85f, 0.6f},
                                   .intensity = 200.0f,
                                   .range = 25.0f,
                                   .inner_angle = 0.15f,
                                   .outer_angle = 0.35f});

        // Blick von rechts vorn auf Raster und Säulen.
        position_ = origin_ + DVec3{13.0, 9.0, 27.0};
        const Vec3 forward = math::normalize(Vec3{-13.0f, -4.0f, -27.0f});
        yaw_ = std::atan2(-forward.x, -forward.z);
        pitch_ = std::asin(forward.y);
        speed_ = 6.0f;
        update(0.0f, {});
        return {};
    }

    // Hüllquader aller Meshes, Lichter als Kugeln, Spotlichter mit Linie, Achsenkreuz im Ursprung.
    void draw_debug() {
        render::DebugDraw& debug = renderer_->debug();
        world_.each<const world::WorldTransform, const render::MeshRenderer>(
            [&](const world::WorldTransform& transform, const render::MeshRenderer& component) {
                if (const render::GpuMesh* mesh = renderer_->find_mesh(component.mesh)) {
                    debug.box(transform.value, mesh->bounds_min, mesh->bounds_max,
                              {0.9f, 0.8f, 0.2f});
                }
            });
        world_.each<const world::WorldTransform, const render::PointLight>(
            [&](const world::WorldTransform& transform, const render::PointLight& light) {
                debug.sphere(transform.position(), 0.35f, light.color, 16);
            });
        world_.each<const world::WorldTransform, const render::SpotLight>(
            [&](const world::WorldTransform& transform, const render::SpotLight& light) {
                const Vec3 direction = -math::normalize(transform.value.linear[2]);
                debug.sphere(transform.position(), 0.35f, light.color, 16);
                debug.line(transform.position(),
                           transform.position() + math::to_f64(direction * light.range),
                           light.color);
            });
        debug.axes(math::Affine::from(origin_, math::quat_identity(), {1.0f, 1.0f, 1.0f}), 2.0f);
    }

    // 7 × 7 Kugeln: Roughness wächst nach rechts, Metallic nach oben.
    void build_spheres() {
        world::Entity root = world_.create("Kugelraster");
        root.set(world::Transform{.position = origin_});
        const f32 half = static_cast<f32>(grid_size - 1) * grid_spacing * 0.5f;
        for (u32 row = 0; row < grid_size; ++row) {
            for (u32 column = 0; column < grid_size; ++column) {
                world::Entity entity = world_.create(std::format("Kugel {} {}", row, column));
                entity
                    .set(world::Transform{
                        .position = {static_cast<f64>(column) * grid_spacing - half,
                                     static_cast<f64>(row) * grid_spacing + 1.2, 0.0}})
                    .set(render::MeshRenderer{
                        .mesh = default_ref(sphere_mesh),
                        .material = default_ref(first_sphere_material + row * grid_size + column)});
                entity.set_parent(root);
            }
        }
    }

    // Kamera vor die Szene stellen, sodass alles ins Bild passt.
    void frame_camera() {
        (void)world::update_transforms(world_);
        const auto bounds = world_bounds(world_, *renderer_, origin_);
        Vec3 center{0.0f, 0.0f, 0.0f};
        f32 radius = 1.0f;
        if (bounds) {
            center = (bounds->first + bounds->second) * 0.5f;
            radius = std::max(math::length(bounds->second - bounds->first) * 0.5f, 0.01f);
        }
        // Boden unter dem Modell, damit es Schatten wirft.
        if (bounds) {
            if (auto result = add_ground(std::max(radius * 6.0f, 10.0f), bounds->first.y);
                !result) {
                log::warn("Boden: {}", result.error());
            }
        }
        renderer_->shadows.distance = std::max(radius * 8.0f, 20.0f);
        const f32 fov = render::Camera{}.fov_y;
        const f32 distance = radius / std::tan(fov * 0.5f);
        position_ = origin_ + math::to_f64(center + Vec3{0.0f, 0.0f, distance});
        speed_ = std::max(radius, 0.5f);
        camera_settings_.near_plane = std::max(radius * 0.01f, 0.01f);
        if (mode_ == SceneMode::Game) {
            camera_.get_mut<render::Camera>()->near_plane = camera_settings_.near_plane;
        }
        update(0.0f, {});
    }

    reflect::TypeRegistry types_;
    world::World world_;
    std::unique_ptr<render::Renderer> renderer_;
    // Wenige Worker reichen für die Systeme der Szene und das Laden im Hintergrund; die
    // Hauptarbeit macht die GPU.
    jobs::JobSystem jobs_;
    // Nach Renderer und Jobs angelegt, deshalb vor ihnen zerstört: Ressourcen geben ihren
    // Speicher im Renderer frei, laufende Jobs müssen fertig sein.
    std::unique_ptr<asset::ResourceManager> resources_;
    world::Schedule schedule_;
    // Zeitschritt für die Systeme in simulate().
    f32 step_ = 0.0f;
    DVec3 origin_;
    SceneMode mode_;
    world::Entity camera_;
    render::Camera camera_settings_;
    DVec3 position_{0.0, 0.0, 0.0};
    f32 yaw_ = 0.0f;
    f32 pitch_ = 0.0f;
    f32 speed_ = 3.0f;
    bool debug_draw_ = false;
};

} // namespace app
