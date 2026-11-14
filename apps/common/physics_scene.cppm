module;

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Physik in Sandbox und Editor (R4.2): Formen aus Modellen für die Physik-Welt, Kollisionsformen
// als Debug-Linien und der Spielplatz der Sandbox (Boden, Rampe, Kistenstapel, Würfe, Regen aus
// vielen Körpern). Der Spielplatz entsteht ohne GPU, damit --headless dieselbe Welt rechnet.
export module app.physics;

import ngine.core;
import ngine.asset;
import ngine.math;
import ngine.physics;
import ngine.render;
import ngine.world;

namespace app {

using namespace ngine;
using math::DVec3;
using math::Vec3;

// Dreiecke der Meshes von Modellen für ConvexHull und Mesh: erzeugte Modelle direkt, Modelle aus
// dem Asset-Ordner gekocht aus dem Cache oder aus der glTF-Quelle. Gelesen wird beim ersten Bedarf
// im Hauptthread und danach gemerkt; die Physik baut daraus ihre Formen einmal pro Skalierung.
export class ModelShapes {
public:
    ModelShapes() = default;
    // database und cache dürfen fehlen und müssen sonst die Formen überleben.
    ModelShapes(const asset::AssetDatabase* database, const asset::AssetCache* cache)
        : database_{database}, cache_{cache} {}

    // Erzeugtes Modell (z. B. der Spielplatz) unter seiner Kennung.
    void add(asset::AssetId id, asset::Model model) {
        models_[id] = std::move(model);
        meshes_.clear();
    }

    // Quelle für physics::PhysicsWorld::set_shape_source(); diese Formen müssen sie überleben.
    [[nodiscard]] physics::ShapeSource source() {
        return [this](u64 model, u32 mesh) { return find(model, mesh); };
    }

    [[nodiscard]] const physics::TriangleMesh* find_mesh(u64 model, u32 mesh) {
        const auto key = std::pair{model, mesh};
        if (const auto found = meshes_.find(key); found != meshes_.end()) {
            return found->second ? &*found->second : nullptr;
        }
        const asset::Model* source = load(model);
        std::optional<physics::TriangleMesh> result;
        if (source != nullptr && mesh < source->meshes.size()) {
            physics::TriangleMesh triangles;
            for (const asset::Primitive& primitive : source->meshes[mesh].primitives) {
                const auto base = static_cast<u32>(triangles.positions.size());
                for (const asset::Vertex& vertex : primitive.vertices) {
                    triangles.positions.push_back(vertex.position);
                }
                for (const u32 index : primitive.indices) {
                    triangles.indices.push_back(base + index);
                }
            }
            result = std::move(triangles);
        }
        const auto& stored = meshes_.emplace(key, std::move(result)).first->second;
        return stored ? &*stored : nullptr;
    }

private:
    std::optional<physics::TriangleMesh> find(u64 model, u32 mesh) {
        const physics::TriangleMesh* triangles = find_mesh(model, mesh);
        if (triangles == nullptr) {
            return std::nullopt;
        }
        return *triangles;
    }

    const asset::Model* load(asset::AssetId id) {
        if (const auto found = models_.find(id); found != models_.end()) {
            return &found->second;
        }
        if (failed_.contains(id) || database_ == nullptr) {
            return nullptr;
        }
        const asset::AssetInfo* info = database_->find(id);
        if (info == nullptr || info->type != asset::AssetType::Model) {
            failed_[id] = true;
            return nullptr;
        }
        if (cache_ != nullptr && cache_->enabled()) {
            if (auto cooked = cache_->load_model(id); cooked && *cooked) {
                return &(models_[id] = std::move((*cooked)->model));
            }
        }
        auto model = asset::load_gltf(info->file);
        if (!model) {
            log::warn("Physik: Modell {} lässt sich nicht lesen: {}", info->path, model.error());
            failed_[id] = true;
            return nullptr;
        }
        return &(models_[id] = *std::move(model));
    }

    const asset::AssetDatabase* database_ = nullptr;
    const asset::AssetCache* cache_ = nullptr;
    std::map<asset::AssetId, asset::Model> models_;
    std::map<asset::AssetId, bool> failed_;
    std::map<std::pair<u64, u32>, std::optional<physics::TriangleMesh>> meshes_;
};

// Kollisionsformen als Linien: grün wach, blau schlafend, grau unbewegt, gelb Auslöser. physics
// darf fehlen (Editor außerhalb des Spielens), dann sind alle Formen grau bzw. gelb.
export void draw_colliders(world::World& world, const physics::PhysicsWorld* physics,
                           ModelShapes* shapes, render::DebugDraw& debug) {
    constexpr Vec3 awake{0.2f, 0.9f, 0.3f};
    constexpr Vec3 sleeping{0.3f, 0.5f, 1.0f};
    constexpr Vec3 still{0.6f, 0.6f, 0.6f};
    constexpr Vec3 trigger{1.0f, 0.85f, 0.2f};
    world.each<const world::WorldTransform, const physics::RigidBody, const physics::Collider>(
        [&](world::Entity entity, const world::WorldTransform& transform,
            const physics::RigidBody& body, const physics::Collider& collider) {
            Vec3 color = still;
            if (collider.sensor || body.layer == physics::PhysicsLayer::Sensor) {
                color = trigger;
            } else if (body.motion != physics::MotionType::Static) {
                color = physics != nullptr && !physics->is_active(entity.id()) ? sleeping : awake;
            }
            const math::Decomposed pose = math::decompose(transform.value);
            const math::Affine frame =
                math::Affine::from(pose.translation, pose.rotation, {1.0f, 1.0f, 1.0f});
            const Vec3 scale{std::abs(pose.scale.x), std::abs(pose.scale.y),
                             std::abs(pose.scale.z)};
            const Vec3 offset{collider.offset.x * pose.scale.x, collider.offset.y * pose.scale.y,
                              collider.offset.z * pose.scale.z};
            const DVec3 center = frame.transform_point(math::to_f64(offset));
            switch (collider.shape) {
            case physics::ShapeType::Box: {
                const Vec3 half{collider.half_extents.x * scale.x,
                                collider.half_extents.y * scale.y,
                                collider.half_extents.z * scale.z};
                debug.box(frame, offset - half, offset + half, color);
                break;
            }
            case physics::ShapeType::Sphere:
                debug.sphere(center, collider.radius * std::max({scale.x, scale.y, scale.z}), color,
                             16);
                break;
            case physics::ShapeType::Capsule: {
                const f32 radius = collider.radius * std::max(scale.x, scale.z);
                const f32 half = collider.half_height * scale.y;
                const Vec3 up = frame.transform_direction({0.0f, half, 0.0f});
                debug.sphere(center + math::to_f64(up), radius, color, 16);
                debug.sphere(center - math::to_f64(up), radius, color, 16);
                for (const Vec3 side : {Vec3{1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f},
                                        Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f}}) {
                    const DVec3 rim = math::to_f64(frame.transform_direction(side * radius));
                    debug.line(center + math::to_f64(up) + rim, center - math::to_f64(up) + rim,
                               color);
                }
                break;
            }
            case physics::ShapeType::ConvexHull:
            case physics::ShapeType::Mesh: {
                const physics::TriangleMesh* mesh =
                    shapes != nullptr ? shapes->find_mesh(collider.model, collider.mesh) : nullptr;
                if (mesh == nullptr) {
                    debug.sphere(center, 0.25f, color, 8);
                    break;
                }
                // Große Netze nur mit ihren ersten Dreiecken, damit die Linien bezahlbar bleiben.
                const usize count = std::min<usize>(mesh->indices.size(), 3 * 4000);
                const auto point = [&](u32 index) {
                    const Vec3 p = mesh->positions[index];
                    return frame.transform_point(math::to_f64(
                        Vec3{p.x * pose.scale.x, p.y * pose.scale.y, p.z * pose.scale.z} + offset));
                };
                if (collider.shape == physics::ShapeType::Mesh) {
                    for (usize i = 0; i + 2 < count; i += 3) {
                        const DVec3 a = point(mesh->indices[i]);
                        const DVec3 b = point(mesh->indices[i + 1]);
                        const DVec3 c = point(mesh->indices[i + 2]);
                        debug.line(a, b, color);
                        debug.line(b, c, color);
                        debug.line(c, a, color);
                    }
                } else {
                    // Hülle: Hüllquader der Punkte reicht als Anhalt.
                    Vec3 low = mesh->positions.empty() ? Vec3{} : mesh->positions.front();
                    Vec3 high = low;
                    for (const Vec3& p : mesh->positions) {
                        low = math::min(low, p);
                        high = math::max(high, p);
                    }
                    const Vec3 s = pose.scale;
                    const Vec3 a{low.x * s.x, low.y * s.y, low.z * s.z};
                    const Vec3 b{high.x * s.x, high.y * s.y, high.z * s.z};
                    debug.box(frame, math::min(a, b) + offset, math::max(a, b) + offset, color);
                }
                break;
            }
            }
        });
}

// Kennung des erzeugten Modells des Spielplatzes.
export constexpr asset::AssetId playground_id = asset::generated_id("app.physics.playground");

// Meshes und Materialien im Modell des Spielplatzes.
export enum PlaygroundMesh : u32 {
    playground_ground,
    playground_box,
    playground_ball,
    playground_ramp
};
enum PlaygroundMaterial : u32 { ground_material, wood_material, ball_material, ramp_material };

constexpr f32 ball_radius = 0.3f;

asset::Material plain_material(std::string name, math::Vec4 color, f32 roughness) {
    asset::Material result;
    result.name = std::move(name);
    result.base_color = color;
    result.metallic = 0.0f;
    result.roughness = roughness;
    return result;
}

// Keil: unten 3 × 6 m, hinten 1,5 m hoch, vorn auf dem Boden. Spitze zeigt nach +z.
asset::Primitive make_ramp() {
    constexpr f32 w = 1.5f;
    constexpr f32 l = 3.0f;
    constexpr f32 h = 1.5f;
    const Vec3 p[6] = {{-w, 0.0f, -l}, {w, 0.0f, -l}, {w, 0.0f, l},
                       {-w, 0.0f, l},  {-w, h, -l},   {w, h, -l}};
    // Flächen mit eigenen Eckpunkten (scharfe Kanten), gegen den Uhrzeigersinn von außen.
    const std::vector<std::vector<u32>> faces = {
        {0, 1, 2, 3}, // unten (wird umgedreht)
        {3, 2, 5, 4}, // schräge Oberseite
        {0, 4, 5, 1}, // hinten
        {0, 3, 4},    // links
        {1, 5, 2},    // rechts
    };
    asset::Primitive primitive;
    for (usize f = 0; f < faces.size(); ++f) {
        std::vector<u32> face = faces[f];
        if (f == 0) {
            std::ranges::reverse(face);
        }
        const auto base = static_cast<u32>(primitive.vertices.size());
        for (const u32 index : face) {
            primitive.vertices.push_back(asset::Vertex{.position = p[index],
                                                       .normal = {0.0f, 1.0f, 0.0f},
                                                       .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
                                                       .uv = {p[index].x, p[index].z}});
        }
        for (u32 i = 1; i + 1 < face.size(); ++i) {
            primitive.indices.push_back(base);
            primitive.indices.push_back(base + i);
            primitive.indices.push_back(base + i + 1);
        }
    }
    asset::generate_normals(primitive.vertices, primitive.indices);
    asset::generate_tangents(primitive.vertices, primitive.indices);
    return primitive;
}

asset::Mesh mesh_of(std::string name, asset::Primitive primitive, u32 material) {
    primitive.material = material;
    asset::Mesh mesh{.name = std::move(name), .primitives = {}};
    mesh.primitives.push_back(std::move(primitive));
    return mesh;
}

// Erzeugtes Modell des Spielplatzes: Boden, Kiste, Kugel, Rampe.
export asset::Model playground_model() {
    asset::Model model;
    model.meshes.push_back(
        mesh_of("Boden", asset::make_box({40.0f, 0.5f, 40.0f}), ground_material));
    model.meshes.push_back(mesh_of("Kiste", asset::make_box({0.5f, 0.5f, 0.5f}), wood_material));
    model.meshes.push_back(
        mesh_of("Kugel", asset::make_sphere(ball_radius, 24, 12), ball_material));
    model.meshes.push_back(mesh_of("Rampe", make_ramp(), ramp_material));
    model.materials.push_back(plain_material("Boden", {0.45f, 0.47f, 0.45f, 1.0f}, 0.9f));
    model.materials.push_back(plain_material("Holz", {0.62f, 0.42f, 0.24f, 1.0f}, 0.7f));
    model.materials.push_back(plain_material("Kugel", {0.85f, 0.2f, 0.15f, 1.0f}, 0.35f));
    model.materials.push_back(plain_material("Rampe", {0.55f, 0.55f, 0.6f, 1.0f}, 0.6f));
    return model;
}

asset::AssetRef playground_ref(PlaygroundMesh mesh) {
    return {.asset = playground_id, .index = mesh};
}

// Eine Kiste zu 1 m an position.
export world::Entity add_box(world::World& world, DVec3 position, std::string name = {}) {
    world::Entity box = name.empty() ? world.create() : world.create(name);
    box.set(world::Transform{.position = position})
        .set(physics::RigidBody{.mass = 20.0f})
        .set(physics::Collider{.shape = physics::ShapeType::Box})
        .set(render::MeshRenderer{.mesh = playground_ref(playground_box)});
    return box;
}

// Eine Kugel, die mit velocity losfliegt.
export world::Entity add_ball(world::World& world, DVec3 position, Vec3 velocity) {
    world::Entity ball = world.create();
    ball.set(world::Transform{.position = position})
        .set(physics::RigidBody{.mass = 5.0f, .restitution = 0.3f, .linear_velocity = velocity})
        .set(physics::Collider{.shape = physics::ShapeType::Sphere, .radius = ball_radius})
        .set(render::MeshRenderer{.mesh = playground_ref(playground_ball)});
    return ball;
}

// Spielplatz um origin (R4.2): Boden, Rampe als Dreiecksnetz, ein Stapel aus 20 Kisten (vier
// Türme zu fünf), ein schiefer Turm aus sechs Kisten und drei Kugeln oben auf der Rampe. Ohne GPU;
// die Meshes kommen aus playground_model().
export void build_playground(world::World& world, DVec3 origin) {
    world.create("Boden")
        .set(world::Transform{.position = origin + DVec3{0.0, -0.5, 0.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static, .friction = 0.8f})
        .set(physics::Collider{.shape = physics::ShapeType::Box,
                               .half_extents = {40.0f, 0.5f, 40.0f}})
        .set(render::MeshRenderer{.mesh = playground_ref(playground_ground)});
    world.create("Rampe")
        .set(world::Transform{.position = origin + DVec3{-8.0, 0.0, 0.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{
            .shape = physics::ShapeType::Mesh, .model = playground_id, .mesh = playground_ramp})
        .set(render::MeshRenderer{.mesh = playground_ref(playground_ramp)});
    world::Entity stack = world.create("Stapel");
    stack.set(world::Transform{.position = origin + DVec3{2.0, 0.0, -2.0}});
    for (i32 tower = 0; tower < 4; ++tower) {
        for (i32 level = 0; level < 5; ++level) {
            const DVec3 place{static_cast<f64>(tower % 2) * 1.05,
                              0.5 + static_cast<f64>(level) * 1.0,
                              static_cast<f64>(tower / 2) * 1.05};
            add_box(world, origin + DVec3{2.0, 0.0, -2.0} + place,
                    std::format("Kiste {}", tower * 5 + level + 1));
        }
    }
    // Ein schiefer Turm daneben fällt von selbst um, sobald die Physik läuft.
    for (i32 level = 0; level < 6; ++level) {
        add_box(world,
                origin + DVec3{6.0 + static_cast<f64>(level) * 0.22,
                               0.5 + static_cast<f64>(level) * 1.0, -2.0},
                std::format("Schiefer Turm {}", level + 1));
    }
    for (i32 i = 0; i < 3; ++i) {
        world::Entity ball =
            add_ball(world, origin + DVec3{-8.5 + static_cast<f64>(i) * 0.8, 2.0, -2.5}, {});
        ball.set(world::Name{std::format("Kugel {}", i + 1)});
    }
}

// Regen aus count Körpern (Kisten und Kugeln im Wechsel) in einem Raster über dem Boden, für die
// Messung mit vielen bewegten Körpern (--bodies).
export void rain_bodies(world::World& world, DVec3 origin, u32 count) {
    const auto side = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(count) / 10.0)));
    for (u32 i = 0; i < count; ++i) {
        const u32 layer = i / (side * side);
        const u32 cell = i % (side * side);
        const DVec3 position = origin + DVec3{(static_cast<f64>(cell % side) - side * 0.5) * 1.6,
                                              6.0 + static_cast<f64>(layer) * 1.6,
                                              (static_cast<f64>(cell / side) - side * 0.5) * 1.6};
        if (i % 2 == 0) {
            (void)add_box(world, position);
        } else {
            (void)add_ball(world, position, {});
        }
    }
}

} // namespace app
