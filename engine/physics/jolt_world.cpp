module;

// Jolt nur in den Dateien jolt*.cpp und jolt.cppm (ADR 0045). Jolt.h zuerst: Es setzt die Schalter,
// die alle anderen Header brauchen.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Körper abgleichen, Schritt, Rückschrieb und Kontakte (PhysicsWorld::Impl) und die
// öffentliche Schnittstelle von PhysicsWorld; Gemeinsames in der internen Partition jolt.cppm.
module ngine.physics;

import ngine.core;
import ngine.math;
import ngine.jobs;
import ngine.profile;
import ngine.world;
import :jolt;

namespace ngine::physics {

PhysicsWorld::Impl::Impl(world::World& w, jobs::JobSystem& j, PhysicsSettings s)
    : world{&w}, jobs{&j}, settings{s} {
    ensure_runtime();
    temp = std::make_unique<JPH::TempAllocatorImpl>(static_cast<JPH::uint>(settings.temp_memory));
    job_adapter = std::make_unique<JobAdapter>(j, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
    create_system();
    transform_id = w.component<world::Transform>();
    body_id = w.component<RigidBody>();
    collider_id = w.component<Collider>();
    interpolated_id = w.component<world::Interpolated>();
    terrain_state_id = w.component<world::TerrainState>();
    character_id = w.component<CharacterController>();
    motion_id = w.component<CharacterMotion>();
    predicted_id = w.component<Predicted>();
    world_transform_id = w.component<world::WorldTransform>();
}

PhysicsWorld::Impl::~Impl() {
    jobs->wait(terrain_jobs);
    remove_all();
    job_adapter->drain();
}

// Neue, leere Jolt-Welt. Jolt ist nur bitgleich, wenn auch der innere Zustand gleich ist
// (Kennungen der Körper, Zwischenspeicher der Kontakte); nach dem Verwerfen aller Körper
// beginnt deshalb eine frische Welt.
void PhysicsWorld::Impl::create_system() {
    system = std::make_unique<JPH::PhysicsSystem>();
    system->Init(settings.max_bodies, 0, settings.max_body_pairs, settings.max_contact_constraints,
                 broad_layers, object_vs_broad, object_pairs);
    system->SetGravity(to_jolt(settings.gravity));
    system->SetContactListener(&contacts);
    for (usize i = 0; i < surfaces.size(); ++i) {
        surfaces[i] = new TerrainSurface{static_cast<voxel::Material>(i)};
    }
    set_terrain_materials(world::standard_terrain_materials());
    contacts.surfaces = &surface_values;
}

JPH::BodyInterface& PhysicsWorld::Impl::body_interface() noexcept {
    return system->GetBodyInterfaceNoLock();
}

void PhysicsWorld::Impl::warn_once(u64 entity, std::string_view message) {
    if (!warned[entity]) {
        warned[entity] = true;
        log::warn("Physik: Entity {}: {}", entity, message);
    }
}

// Weltlage einer Entity (Position, Drehung, Skalierung).
math::Decomposed PhysicsWorld::Impl::world_pose(u64 entity, const world::Transform& transform,
                                                bool& has_parent) {
    has_parent = world->parent(entity) != 0;
    if (!has_parent) {
        return math::Decomposed{.translation = transform.position,
                                .rotation = transform.rotation,
                                .scale = transform.scale};
    }
    return math::decompose(world::world_affine(*world, entity));
}

JPH::Ref<JPH::Shape> PhysicsWorld::Impl::model_shape(u64 entity, const Collider& collider,
                                                     ShapeType type, math::Vec3 scale) {
    const ShapeKey key{.model = collider.model,
                       .mesh = collider.mesh,
                       .type = type,
                       .scale_x = std::bit_cast<u32>(scale.x),
                       .scale_y = std::bit_cast<u32>(scale.y),
                       .scale_z = std::bit_cast<u32>(scale.z)};
    if (const auto found = shape_cache.find(key); found != shape_cache.end()) {
        return found->second;
    }
    if (!shape_source || collider.model == 0) {
        warn_once(entity, "Form aus einem Modell ohne Modell oder ohne ShapeSource");
        return nullptr;
    }
    std::optional<TriangleMesh> mesh = shape_source(collider.model, collider.mesh);
    if (!mesh) {
        // Noch nicht geladen: Der nächste Abgleich versucht es wieder.
        return nullptr;
    }
    JPH::Ref<JPH::Shape> shape;
    if (type == ShapeType::ConvexHull) {
        JPH::Array<JPH::Vec3> points;
        points.reserve(mesh->positions.size());
        for (const math::Vec3& p : mesh->positions) {
            points.push_back(JPH::Vec3{p.x * scale.x, p.y * scale.y, p.z * scale.z});
        }
        JPH::ConvexHullShapeSettings hull{points};
        shape = created(hull.Create(), "Hülle");
    } else {
        JPH::VertexList vertices;
        vertices.reserve(mesh->positions.size());
        for (const math::Vec3& p : mesh->positions) {
            vertices.push_back(JPH::Float3{p.x * scale.x, p.y * scale.y, p.z * scale.z});
        }
        JPH::IndexedTriangleList triangles;
        triangles.reserve(mesh->indices.size() / 3);
        const auto count = static_cast<u32>(mesh->positions.size());
        for (usize i = 0; i + 2 < mesh->indices.size(); i += 3) {
            const u32 a = mesh->indices[i];
            const u32 b = mesh->indices[i + 1];
            const u32 c = mesh->indices[i + 2];
            if (a < count && b < count && c < count) {
                triangles.push_back(JPH::IndexedTriangle{a, b, c, 0});
            }
        }
        // Spiegelnde Skalierung dreht den Umlaufsinn um.
        if (scale.x * scale.y * scale.z < 0.0f) {
            for (JPH::IndexedTriangle& triangle : triangles) {
                std::swap(triangle.mIdx[1], triangle.mIdx[2]);
            }
        }
        JPH::MeshShapeSettings mesh_settings{std::move(vertices), std::move(triangles)};
        shape = created(mesh_settings.Create(), "Netz");
    }
    if (shape != nullptr) {
        shape_cache.emplace(key, shape);
    }
    return shape;
}

JPH::Ref<JPH::Shape> PhysicsWorld::Impl::make_shape(u64 entity, const RigidBody& body,
                                                    const Collider& collider, math::Vec3 scale) {
    JPH::Ref<JPH::Shape> shape;
    ShapeType type = collider.shape;
    if (type == ShapeType::Mesh && body.motion == MotionType::Dynamic) {
        // Jolt rechnet Dreiecksnetze nur unbewegt oder kinematisch.
        warn_once(entity, "dynamischer Körper mit Dreiecksnetz, nutzt die konvexe Hülle");
        type = ShapeType::ConvexHull;
    }
    if (type == ShapeType::ConvexHull || type == ShapeType::Mesh) {
        shape = model_shape(entity, collider, type, scale);
    } else {
        shape = primitive_shape(type, collider.radius, collider.half_extents, collider.half_height,
                                scale);
    }
    if (shape == nullptr) {
        return nullptr;
    }
    if (!same(collider.offset, math::Vec3{0.0f, 0.0f, 0.0f})) {
        const JPH::Vec3 offset{collider.offset.x * scale.x, collider.offset.y * scale.y,
                               collider.offset.z * scale.z};
        JPH::RotatedTranslatedShapeSettings moved{offset, JPH::Quat::sIdentity(), shape};
        shape = created(moved.Create(), "Versatz");
    }
    return shape;
}

// Legt den Körper an; false, wenn die Form (noch) fehlt.
bool PhysicsWorld::Impl::create_body(u64 entity, const RigidBody& body, const Collider& collider,
                                     const world::Transform& transform) {
    bool has_parent = false;
    const math::Decomposed pose = world_pose(entity, transform, has_parent);
    JPH::Ref<JPH::Shape> shape = make_shape(entity, body, collider, pose.scale);
    if (shape == nullptr) {
        return false;
    }
    const bool sensor = collider.sensor || body.layer == PhysicsLayer::Sensor;
    PhysicsLayer layer = body.layer;
    if (sensor) {
        layer = PhysicsLayer::Sensor;
    } else if (body.motion == MotionType::Static) {
        layer = PhysicsLayer::Static;
    } else if (layer == PhysicsLayer::Static) {
        layer = PhysicsLayer::Moving;
    }
    const JPH::EMotionType motion = body.motion == MotionType::Static ? JPH::EMotionType::Static
                                    : body.motion == MotionType::Kinematic
                                        ? JPH::EMotionType::Kinematic
                                        : JPH::EMotionType::Dynamic;
    JPH::BodyCreationSettings creation{shape.GetPtr(), to_jolt(pose.translation),
                                       to_jolt(math::normalize(pose.rotation)), motion,
                                       object_layer(layer)};
    creation.mUserData = entity;
    creation.mIsSensor = sensor;
    creation.mFriction = body.friction;
    creation.mRestitution = body.restitution;
    creation.mLinearDamping = body.linear_damping;
    creation.mAngularDamping = body.angular_damping;
    creation.mGravityFactor = body.gravity_factor;
    creation.mAllowSleeping = body.allow_sleep;
    if (body.motion != MotionType::Static) {
        creation.mLinearVelocity = to_jolt(body.linear_velocity);
        creation.mAngularVelocity = to_jolt(body.angular_velocity);
    }
    if (sensor && body.motion != MotionType::Dynamic) {
        // Unbewegte Sensoren sollen auch ruhende und kinematische Körper sehen.
        creation.mCollideKinematicVsNonDynamic = true;
    }
    if (body.mass > 0.0f && body.motion == MotionType::Dynamic) {
        creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        creation.mMassPropertiesOverride.mMass = body.mass;
    }
    JPH::Body* created_body = body_interface().CreateBody(creation);
    if (created_body == nullptr) {
        warn_once(entity, "kein Platz mehr für Körper (PhysicsSettings::max_bodies)");
        return false;
    }
    const JPH::BodyID id = created_body->GetID();
    const bool active = body.motion != MotionType::Static;
    body_interface().AddBody(id,
                             active ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    bodies[entity] = BodyRecord{.id = id,
                                .body = body,
                                .collider = collider,
                                .transform = transform,
                                .has_parent = has_parent};
    entity_of_body[id.GetIndexAndSequenceNumber()] = entity;
    return true;
}

void PhysicsWorld::Impl::destroy_body(std::map<u64, BodyRecord>::iterator it,
                                      std::vector<ContactEnded>& ended,
                                      std::vector<TriggerLeft>& left) {
    const u64 entity = it->first;
    const JPH::BodyID id = it->second.id;
    body_interface().RemoveBody(id);
    body_interface().DestroyBody(id);
    entity_of_body.erase(id.GetIndexAndSequenceNumber());
    loose_proxies.erase(id.GetIndexAndSequenceNumber());
    frozen.erase(entity);
    // Laufende Kontakte des Körpers enden mit ihm.
    for (auto pair = touching.begin(); pair != touching.end();) {
        if (pair->first.first == entity || pair->first.second == entity) {
            emit_end(pair->first.first, pair->first.second, ended, left);
            pair = touching.erase(pair);
        } else {
            ++pair;
        }
    }
    bodies.erase(it);
}

bool PhysicsWorld::Impl::is_sensor_entity(u64 entity) const {
    const auto found = bodies.find(entity);
    if (found == bodies.end()) {
        return false;
    }
    return found->second.collider.sensor || found->second.body.layer == PhysicsLayer::Sensor;
}

void PhysicsWorld::Impl::emit_end(u64 a, u64 b, std::vector<ContactEnded>& ended,
                                  std::vector<TriggerLeft>& left) {
    if (is_sensor_entity(a)) {
        left.push_back(TriggerLeft{.sensor = a, .other = b});
    } else if (is_sensor_entity(b)) {
        left.push_back(TriggerLeft{.sensor = b, .other = a});
    } else {
        ended.push_back(ContactEnded{.a = a, .b = b});
    }
}

void PhysicsWorld::Impl::remove_all() {
    std::vector<ContactEnded> ended;
    std::vector<TriggerLeft> left;
    while (!bodies.empty()) {
        destroy_body(bodies.begin(), ended, left);
    }
    while (!characters.empty()) {
        destroy_character(characters.begin(), ended, left);
    }
    for (auto& [entity, terrain] : terrains) {
        for (auto& [coord, chunk] : terrain.chunks) {
            remove_chunk_body(chunk);
        }
    }
    terrains.clear();
    frozen.clear();
    touching.clear();
    (void)contacts.take();
}

void PhysicsWorld::Impl::clear() {
    jobs->wait(terrain_jobs);
    remove_all();
    create_system();
    shape_cache.clear();
    warned.clear();
}

void PhysicsWorld::Impl::sync(std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left,
                              f32 dt) {
    NGINE_PROFILE_ZONE("PhysicsWorld::sync");
    // Gewünschte Körper mit ihren Werten. Im Modus remote (Client, R5.3) wird Dynamisches
    // kinematisch, und Figuren, die der Client nicht vorhersagt, werden kinematische Kapseln.
    struct Wanted {
        u64 entity = 0;
        RigidBody body;
        Collider collider;
        // Stellvertreter auf einem Client: ohne Interpolation, das Transform kommt vom Server.
        bool proxy = false;
        // Stellvertreter eines Körpers, der auf dem Server dynamisch ist.
        bool loose = false;
    };
    std::vector<Wanted> wanted;
    const bool remote = settings.remote;
    world->each<const RigidBody, const Collider, const world::Transform>(
        [&](world::Entity entity, const RigidBody& body, const Collider& collider,
            const world::Transform&) {
            // Eine Spielfigur ist ihr eigener Körper.
            if (world->has(entity.id(), character_id)) {
                return;
            }
            Wanted entry{.entity = entity.id(), .body = body, .collider = collider};
            if (remote && body.motion == MotionType::Dynamic) {
                entry.body.motion = MotionType::Kinematic;
                entry.proxy = true;
                entry.loose = true;
            }
            wanted.push_back(entry);
        });
    if (remote) {
        world->each<const CharacterController, const world::Transform>(
            [&](world::Entity entity, const CharacterController& controller,
                const world::Transform&) {
                if (world->has(entity.id(), predicted_id)) {
                    return;
                }
                const f32 radius = std::max(controller.radius, 0.01f);
                const f32 height = std::max(controller.height, 2.0f * radius);
                wanted.push_back(Wanted{.entity = entity.id(),
                                        .body = RigidBody{.motion = MotionType::Kinematic,
                                                          .layer = PhysicsLayer::Character},
                                        .collider = Collider{.shape = ShapeType::Capsule,
                                                             .radius = radius,
                                                             .half_height = 0.5f * height - radius,
                                                             .offset = {0.0f, 0.5f * height, 0.0f}},
                                        .proxy = true});
            });
    }
    std::ranges::sort(wanted, {}, &Wanted::entity);
    const auto wanted_entity = [&](u64 entity) {
        return std::ranges::binary_search(wanted, entity, {}, &Wanted::entity);
    };

    // Weggefallene Körper zuerst, in der Reihenfolge der Kennungen.
    for (auto it = bodies.begin(); it != bodies.end();) {
        if (!wanted_entity(it->first)) {
            auto next = std::next(it);
            destroy_body(it, ended, left);
            it = next;
        } else {
            ++it;
        }
    }

    // Interpolation für bewegliche Körper: Strukturänderung vor allen Zeigern in die Welt.
    for (const Wanted& entry : wanted) {
        if (entry.proxy || bodies.contains(entry.entity) ||
            world->has(entry.entity, interpolated_id)) {
            continue;
        }
        if (entry.body.motion != MotionType::Static) {
            (void)world->ensure(entry.entity, interpolated_id);
            world->modified(entry.entity, interpolated_id);
        }
    }

    usize added = 0;
    for (const Wanted& entry : wanted) {
        const u64 entity = entry.entity;
        const auto* transform =
            static_cast<const world::Transform*>(world->get(entity, transform_id));
        auto found = bodies.find(entity);
        if (found != bodies.end()) {
            BodyRecord& record = found->second;
            const bool rebuild = !same_settings(record.body, entry.body) ||
                                 !same_collider(record.collider, entry.collider) ||
                                 !same(record.transform.scale, transform->scale);
            if (rebuild) {
                destroy_body(found, ended, left);
            } else {
                update_body(entity, record, entry.body, *transform, dt);
                continue;
            }
        }
        if (create_body(entity, entry.body, entry.collider, *transform)) {
            ++added;
            if (entry.loose) {
                // Masse wie auf dem Server: angegeben oder aus der Form (Dichte von Wasser).
                const JPH::BodyID id = bodies.at(entity).id;
                f32 mass = entry.body.mass;
                if (mass <= 0.0f) {
                    mass = body_interface().GetShape(id)->GetMassProperties().mMass;
                }
                loose_proxies[id.GetIndexAndSequenceNumber()] = mass;
            }
        }
    }
    if (added > 64) {
        // Viele neue Körper auf einmal: Breitenphase neu ordnen, sonst sind die ersten Schritte
        // langsam.
        system->OptimizeBroadPhase();
    }
}

// Änderungen von außen an einem bestehenden Körper.
void PhysicsWorld::Impl::update_body(u64 entity, BodyRecord& record, const RigidBody& body,
                                     const world::Transform& transform, f32 dt) {
    JPH::BodyInterface& bodies_interface = body_interface();
    const bool moved = !same(record.transform, transform);
    if (body.motion == MotionType::Kinematic) {
        // Kinematische Körper fahren zu ihrem Transform; steht er, kommen sie zur Ruhe.
        if (moved || bodies_interface.IsActive(record.id)) {
            bool has_parent = false;
            const math::Decomposed pose = world_pose(entity, transform, has_parent);
            bodies_interface.MoveKinematic(record.id, to_jolt(pose.translation),
                                           to_jolt(math::normalize(pose.rotation)), dt);
        }
        record.transform = transform;
        return;
    }
    if (moved) {
        bool has_parent = false;
        const math::Decomposed pose = world_pose(entity, transform, has_parent);
        bodies_interface.SetPositionAndRotation(
            record.id, to_jolt(pose.translation), to_jolt(math::normalize(pose.rotation)),
            body.motion == MotionType::Static ? JPH::EActivation::DontActivate
                                              : JPH::EActivation::Activate);
        record.transform = transform;
    }
    if (body.motion == MotionType::Dynamic && frozen.contains(entity)) {
        // Eingefroren: Die Geschwindigkeit gilt beim Auftauen.
        record.body.linear_velocity = body.linear_velocity;
        record.body.angular_velocity = body.angular_velocity;
        return;
    }
    if (body.motion == MotionType::Dynamic &&
        (!same(record.body.linear_velocity, body.linear_velocity) ||
         !same(record.body.angular_velocity, body.angular_velocity))) {
        bodies_interface.SetLinearAndAngularVelocity(record.id, to_jolt(body.linear_velocity),
                                                     to_jolt(body.angular_velocity));
        if (!same(body.linear_velocity, math::Vec3{}) ||
            !same(body.angular_velocity, math::Vec3{})) {
            bodies_interface.ActivateBody(record.id);
        }
        record.body.linear_velocity = body.linear_velocity;
        record.body.angular_velocity = body.angular_velocity;
    }
}

void PhysicsWorld::Impl::write_back() {
    NGINE_PROFILE_ZONE("PhysicsWorld::write_back");
    JPH::BodyInterface& bodies_interface = body_interface();
    u32 active = 0;
    for (auto& [entity, record] : bodies) {
        if (record.body.motion != MotionType::Dynamic || frozen.contains(entity)) {
            continue;
        }
        const bool awake = bodies_interface.IsActive(record.id);
        auto* body = static_cast<RigidBody*>(world->get_mut(entity, body_id));
        if (!awake) {
            // Eingeschlafen: Jolt hat die Geschwindigkeit auf null gesetzt.
            if (!same(record.body.linear_velocity, math::Vec3{}) ||
                !same(record.body.angular_velocity, math::Vec3{})) {
                record.body.linear_velocity = {};
                record.body.angular_velocity = {};
                body->linear_velocity = {};
                body->angular_velocity = {};
            }
            continue;
        }
        ++active;
        JPH::RVec3 position;
        JPH::Quat rotation;
        bodies_interface.GetPositionAndRotation(record.id, position, rotation);
        JPH::Vec3 linear;
        JPH::Vec3 angular;
        bodies_interface.GetLinearAndAngularVelocity(record.id, linear, angular);
        body->linear_velocity = from_jolt(linear);
        body->angular_velocity = from_jolt(angular);
        record.body.linear_velocity = body->linear_velocity;
        record.body.angular_velocity = body->angular_velocity;

        auto* transform = static_cast<world::Transform*>(world->get_mut(entity, transform_id));
        if (!record.has_parent) {
            transform->position = from_jolt(position);
            transform->rotation = from_jolt(rotation);
        } else {
            // Lokale Lage aus der Weltlage und der des Elternteils.
            const u64 parent = world->parent(entity);
            const math::Affine global =
                math::Affine::from(from_jolt(position), from_jolt(rotation), {1.0f, 1.0f, 1.0f});
            const math::Decomposed local =
                math::decompose(math::inverse(world::world_affine(*world, parent)) * global);
            transform->position = local.translation;
            transform->rotation = local.rotation;
        }
        world->modified(entity, transform_id);
        record.transform = *transform;
    }
    stats.active_bodies = active;
}

void PhysicsWorld::Impl::publish(std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left) {
    NGINE_PROFILE_ZONE("PhysicsWorld::contacts");
    // Jolt meldet Kontakte pro Teilform und in wechselnder Reihenfolge. Pro Paar zählt nur, ob
    // die beiden Körper nach dem Schritt in Berührung sind; als Punkt gilt der kleinste
    // gemeldete, damit das Ergebnis nicht von der Reihenfolge der Threads abhängt.
    struct Change {
        bool has_point = false;
        math::DVec3 point;
        math::Vec3 normal;
    };
    std::map<std::pair<u64, u64>, Change> changes;
    for (const ContactRecord& record : contacts.take()) {
        const auto first = entity_of_body.find(record.body1);
        const auto second = entity_of_body.find(record.body2);
        if (first == entity_of_body.end() || second == entity_of_body.end()) {
            continue;
        }
        u64 a = first->second;
        u64 b = second->second;
        // Jolts Normale zeigt von Körper 1 nach 2; gemeldet wird sie von a nach b.
        math::Vec3 normal = from_jolt(record.normal);
        if (b < a) {
            std::swap(a, b);
            normal = -normal;
        }
        Change& change = changes[{a, b}];
        if (record.added) {
            const math::DVec3 point = from_jolt(record.point);
            if (!change.has_point || std::tie(point.x, point.y, point.z) <
                                         std::tie(change.point.x, change.point.y, change.point.z)) {
                change.point = point;
                change.normal = normal;
                change.has_point = true;
            }
        }
    }

    JPH::BodyInterface& bodies_interface = body_interface();
    const auto body_of = [&](u64 entity) { return body_id_of(entity); };
    const auto in_contact = [&](u64 a, u64 b) {
        return system->WereBodiesInContact(body_of(a), body_of(b));
    };
    const auto asleep = [&](u64 a, u64 b) {
        return !bodies_interface.IsActive(body_of(a)) && !bodies_interface.IsActive(body_of(b));
    };

    std::vector<ContactBegan> began;
    std::vector<TriggerEntered> entered;
    for (const auto& [pair, change] : changes) {
        const auto found = touching.find(pair);
        if (in_contact(pair.first, pair.second)) {
            if (found == touching.end()) {
                if (is_sensor_entity(pair.first)) {
                    entered.push_back(TriggerEntered{.sensor = pair.first, .other = pair.second});
                } else if (is_sensor_entity(pair.second)) {
                    entered.push_back(TriggerEntered{.sensor = pair.second, .other = pair.first});
                } else {
                    began.push_back(ContactBegan{.a = pair.first,
                                                 .b = pair.second,
                                                 .point = change.point,
                                                 .normal = change.normal});
                }
            }
            touching[pair] = false;
        } else if (found != touching.end()) {
            if (asleep(pair.first, pair.second)) {
                // Eingeschlafen, nicht getrennt.
                found->second = true;
            } else {
                emit_end(pair.first, pair.second, ended, left);
                touching.erase(found);
            }
        }
    }
    // Ruhende Paare, von denen einer aufgewacht ist, ohne dass Jolt etwas meldet: Sind sie
    // noch in Berührung, laufen sie weiter, sonst endet der Kontakt.
    for (auto it = touching.begin(); it != touching.end();) {
        if (it->second && !asleep(it->first.first, it->first.second)) {
            if (in_contact(it->first.first, it->first.second)) {
                it->second = false;
            } else {
                emit_end(it->first.first, it->first.second, ended, left);
                it = touching.erase(it);
                continue;
            }
        }
        ++it;
    }
    stats.contacts = static_cast<u32>(touching.size());
    if (bus == nullptr) {
        return;
    }
    for (const ContactEnded& event : ended) {
        bus->publish(event);
    }
    for (const TriggerLeft& event : left) {
        bus->publish(event);
    }
    for (const ContactBegan& event : began) {
        bus->publish(event);
    }
    for (const TriggerEntered& event : entered) {
        bus->publish(event);
    }
    for (const CharacterLanded& event : landed_events) {
        bus->publish(event);
    }
    for (const CharacterStep& event : step_events) {
        bus->publish(event);
    }
}

void PhysicsWorld::Impl::set_terrain_materials(const world::TerrainMaterials& table) {
    for (usize i = 0; i < surface_values.size(); ++i) {
        const world::TerrainMaterial* material = table.find(static_cast<u32>(i));
        surface_values[i] = material != nullptr
                                ? SurfaceValues{.friction = material->friction,
                                                .restitution = material->restitution}
                                : SurfaceValues{};
    }
}

// -------------------------------------------------------------------------------------------------

PhysicsWorld::PhysicsWorld(world::World& world, jobs::JobSystem& jobs, PhysicsSettings settings)
    : impl_{std::make_unique<Impl>(world, jobs, settings)} {}

PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::set_shape_source(ShapeSource source) {
    impl_->shape_source = std::move(source);
    impl_->shape_cache.clear();
}

void PhysicsWorld::set_terrain_materials(const world::TerrainMaterials& table) {
    impl_->set_terrain_materials(table);
}

void PhysicsWorld::set_wait_for_terrain(bool wait) noexcept {
    impl_->settings.wait_for_terrain = wait;
}

void PhysicsWorld::set_wait_for_rebuilds(bool wait) noexcept {
    impl_->settings.wait_for_rebuilds = wait;
}

void PhysicsWorld::set_remote(bool remote) noexcept {
    impl_->settings.remote = remote;
}

bool PhysicsWorld::remote() const noexcept {
    return impl_->settings.remote;
}

void PhysicsWorld::set_events(events::EventBus* bus) noexcept {
    impl_->bus = bus;
}

void PhysicsWorld::sync() {
    std::vector<ContactEnded> ended;
    std::vector<TriggerLeft> left;
    impl_->sync(ended, left, impl_->world->time().fixed_delta);
    if (impl_->bus != nullptr) {
        for (const ContactEnded& event : ended) {
            impl_->bus->publish(event);
        }
        for (const TriggerLeft& event : left) {
            impl_->bus->publish(event);
        }
    }
}

void PhysicsWorld::step(f32 dt) {
    NGINE_PROFILE_ZONE("PhysicsWorld::step");
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::duration d) {
        return std::chrono::duration<f64, std::milli>(d).count();
    };
    Impl& impl = *impl_;
    std::vector<ContactEnded> ended;
    std::vector<TriggerLeft> left;

    const auto start = clock::now();
    impl.sync(ended, left, dt);
    impl.sync_characters(ended, left);
    impl.sync_terrains();
    impl.update_frozen();
    const auto synced = clock::now();
    impl.update_characters(dt);
    impl.stats.characters_ms = ms(clock::now() - synced);
    {
        NGINE_PROFILE_ZONE("PhysicsSystem::Update");
        const JPH::EPhysicsUpdateError error = impl.system->Update(
            dt, static_cast<int>(std::max<u32>(impl.settings.collision_steps, 1)), impl.temp.get(),
            impl.job_adapter.get());
        impl.job_adapter->drain();
        if (error != JPH::EPhysicsUpdateError::None) {
            log::warn("Physik: Schritt mit Fehler {} (Grenzen in PhysicsSettings erhöhen)",
                      static_cast<u32>(error));
        }
    }
    const auto updated = clock::now();
    impl.write_back();
    impl.publish(ended, left);
    const auto written = clock::now();

    impl.stats.sync_ms = ms(synced - start);
    impl.stats.update_ms = ms(updated - synced);
    impl.stats.write_back_ms = ms(written - updated);
    impl.stats.bodies = static_cast<u32>(impl.bodies.size());
    impl.stats.characters = static_cast<u32>(impl.characters.size());
}

void PhysicsWorld::clear() {
    impl_->clear();
}

bool PhysicsWorld::has_body(u64 entity) const noexcept {
    return impl_->bodies.contains(entity);
}

bool PhysicsWorld::is_active(u64 entity) const noexcept {
    const auto found = impl_->bodies.find(entity);
    if (found == impl_->bodies.end()) {
        return false;
    }
    return impl_->system->GetBodyInterfaceNoLock().IsActive(found->second.id);
}

bool PhysicsWorld::is_frozen(u64 entity) const noexcept {
    return impl_->frozen.contains(entity);
}

std::vector<TerrainChunkShape> PhysicsWorld::terrain_shapes() const {
    std::vector<TerrainChunkShape> result;
    for (const auto& [entity, terrain] : impl_->terrains) {
        for (const auto& [coord, chunk] : terrain.chunks) {
            if (chunk.body.IsInvalid()) {
                continue;
            }
            result.push_back(
                TerrainChunkShape{.terrain = entity,
                                  .x = coord.x,
                                  .y = coord.y,
                                  .z = coord.z,
                                  .corner = terrain.base + voxel::chunk_position(coord),
                                  .size = static_cast<f32>(voxel::chunk_size),
                                  .triangles = chunk.triangles,
                                  .rebuilding = chunk.build != nullptr});
        }
    }
    return result;
}

usize PhysicsWorld::body_count() const noexcept {
    return impl_->bodies.size();
}

const StepStats& PhysicsWorld::stats() const noexcept {
    return impl_->stats;
}

void add_to(world::Simulation& simulation, PhysicsWorld& physics) {
    world::World* world = &simulation.world();
    simulation.add_fixed_stage([world, &physics] { physics.step(world->time().fixed_delta); });
}

u64 checksum(world::World& world) {
    struct Entry {
        u64 entity;
        world::Transform transform;
        RigidBody body;
    };
    std::vector<Entry> entries;
    world.each<const RigidBody, const world::Transform>(
        [&](world::Entity entity, const RigidBody& body, const world::Transform& transform) {
            entries.push_back(Entry{.entity = entity.id(), .transform = transform, .body = body});
        });
    std::ranges::sort(entries, {}, &Entry::entity);
    u64 hash = 14695981039346656037ull;
    const auto feed = [&hash](const void* data, usize size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (usize i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    for (const Entry& entry : entries) {
        feed(&entry.transform.position, sizeof(entry.transform.position));
        feed(&entry.transform.rotation, sizeof(entry.transform.rotation));
        feed(&entry.body.linear_velocity, sizeof(entry.body.linear_velocity));
        feed(&entry.body.angular_velocity, sizeof(entry.body.angular_velocity));
    }
    return hash;
}

} // namespace ngine::physics
