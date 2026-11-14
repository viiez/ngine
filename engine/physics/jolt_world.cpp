module;

// Jolt nur hier (ADR 0045). Jolt.h zuerst: Es setzt die Schalter, die alle anderen Header brauchen.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
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
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

module ngine.physics;

import ngine.core;
import ngine.math;
import ngine.jobs;
import ngine.profile;
import ngine.world;

namespace ngine::physics {

namespace {

// ---------------------------------------------------------------------------------------------
// Laufzeit von Jolt: einmal pro Prozess (Allokator, Fabrik, Typen).

void trace(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    char buffer[1024];
    (void)std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    log::warn("Jolt: {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool assert_failed(const char* expression, const char* message, const char* file, JPH::uint line) {
    log::error("Jolt: Annahme verletzt: {} ({}) in {}:{}", expression,
               message != nullptr ? message : "", file, line);
    return true;
}
#endif

class JoltRuntime {
public:
    JoltRuntime() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = &trace;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = &assert_failed;
#endif
        // Bricht ab, wenn Bibliothek und Adapter mit anderen Schaltern gebaut sind (z. B. ohne
        // doppelte Genauigkeit).
        verify(JPH::VerifyJoltVersionID(), "Jolt: Bibliothek und Adapter passen nicht zusammen");
        factory_ = std::make_unique<JPH::Factory>();
        JPH::Factory::sInstance = factory_.get();
        JPH::RegisterTypes();
    }
    ~JoltRuntime() {
        JPH::UnregisterTypes();
        JPH::Factory::sInstance = nullptr;
    }
    JoltRuntime(const JoltRuntime&) = delete;
    JoltRuntime& operator=(const JoltRuntime&) = delete;

private:
    std::unique_ptr<JPH::Factory> factory_;
};

void ensure_runtime() {
    static JoltRuntime runtime;
}

// ---------------------------------------------------------------------------------------------
// Job-System: Jolts Jobs laufen als Jobs von ngine.jobs, damit nicht zwei Thread-Pools um die
// Kerne streiten. Auf Barrieren wartet Jolt selbst und rechnet dabei bereite Jobs mit.

class JobAdapter final : public JPH::JobSystemWithBarrier {
public:
    JobAdapter(jobs::JobSystem& jobs, JPH::uint max_jobs, JPH::uint max_barriers)
        : JobSystemWithBarrier{max_barriers}, jobs_{&jobs} {
        free_list_.Init(max_jobs, max_jobs);
    }
    ~JobAdapter() override {
        drain();
    }
    JobAdapter(const JobAdapter&) = delete;
    JobAdapter& operator=(const JobAdapter&) = delete;

    int GetMaxConcurrency() const override {
        return static_cast<int>(jobs_->worker_count()) + 1;
    }

    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                        JPH::uint32 dependencies) override {
        JPH::uint32 index = AvailableJobs::cInvalidObjectIndex;
        for (;;) {
            index = free_list_.ConstructObject(name, color, this, function, dependencies);
            if (index != AvailableJobs::cInvalidObjectIndex) {
                break;
            }
            std::this_thread::yield();
        }
        Job* job = &free_list_.Get(index);
        // Der Griff hält eine Referenz; der Job kann gleich nach dem Einreihen fertig sein.
        JobHandle handle{job};
        if (dependencies == 0) {
            QueueJob(job);
        }
        return handle;
    }

    // Wartet, bis alle eingereihten Jobs von ngine.jobs gelaufen sind. Jobs, die Jolt beim Warten
    // auf eine Barriere schon selbst gerechnet hat, sind dann leere Aufrufe; danach hält kein Job
    // von ngine.jobs mehr einen Zeiger in diesen Adapter.
    void drain() {
        jobs_->wait(pending_);
    }

protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        jobs_->run(
            [job] {
                (void)job->Execute();
                job->Release();
            },
            &pending_);
    }
    void QueueJobs(Job** jobs, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) {
            QueueJob(jobs[i]);
        }
    }
    void FreeJob(Job* job) override {
        free_list_.DestructObject(job);
    }

private:
    using AvailableJobs = JPH::FixedSizeFreeList<Job>;
    jobs::JobSystem* jobs_;
    AvailableJobs free_list_;
    jobs::Counter pending_;
};

// ---------------------------------------------------------------------------------------------
// Ebenen: Objektebenen sind die PhysicsLayer; die Breitenphase kennt nur unbewegt und bewegt.

constexpr JPH::BroadPhaseLayer broad_static{0};
constexpr JPH::BroadPhaseLayer broad_moving{1};
constexpr JPH::uint broad_count = 2;

JPH::ObjectLayer object_layer(PhysicsLayer layer) noexcept {
    return static_cast<JPH::ObjectLayer>(layer);
}
PhysicsLayer physics_layer(JPH::ObjectLayer layer) noexcept {
    return static_cast<PhysicsLayer>(layer);
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override {
        return broad_count;
    }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return physics_layer(layer) == PhysicsLayer::Static ? broad_static : broad_moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == broad_static ? "Static" : "Moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        const PhysicsLayer own = physics_layer(layer);
        if (broad == broad_static) {
            return layers_collide(own, PhysicsLayer::Static);
        }
        return own != PhysicsLayer::Static;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return layers_collide(physics_layer(a), physics_layer(b));
    }
};

// Filter der Abfragen.
class QueryBroadFilter final : public JPH::BroadPhaseLayerFilter {
public:
    explicit QueryBroadFilter(u32 layers) noexcept : layers_{layers} {}
    bool ShouldCollide(JPH::BroadPhaseLayer broad) const override {
        if (broad == broad_static) {
            return (layers_ & layer_bit(PhysicsLayer::Static)) != 0;
        }
        return (layers_ & ~layer_bit(PhysicsLayer::Static)) != 0;
    }

private:
    u32 layers_;
};

class QueryObjectFilter final : public JPH::ObjectLayerFilter {
public:
    explicit QueryObjectFilter(u32 layers) noexcept : layers_{layers} {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return (layers_ & (u32{1} << layer)) != 0;
    }

private:
    u32 layers_;
};

class QueryBodyFilter final : public JPH::BodyFilter {
public:
    QueryBodyFilter(u64 ignore, bool sensors) noexcept : ignore_{ignore}, sensors_{sensors} {}
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (ignore_ != 0 && body.GetUserData() == ignore_) {
            return false;
        }
        return sensors_ || !body.IsSensor();
    }

private:
    u64 ignore_;
    bool sensors_;
};

// ---------------------------------------------------------------------------------------------
// Kontakte: Jolt meldet sie aus mehreren Threads; gesammelt und nach dem Schritt sortiert.

struct ContactRecord {
    JPH::uint32 body1 = 0;
    JPH::uint32 body2 = 0;
    bool added = true;
    JPH::RVec3 point;
    JPH::Vec3 normal;
};

class ContactCollector final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        const JPH::RVec3 point = manifold.GetWorldSpaceContactPointOn1(0);
        const std::scoped_lock lock{mutex_};
        records_.push_back(ContactRecord{.body1 = body1.GetID().GetIndexAndSequenceNumber(),
                                         .body2 = body2.GetID().GetIndexAndSequenceNumber(),
                                         .added = true,
                                         .point = point,
                                         .normal = manifold.mWorldSpaceNormal});
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        const std::scoped_lock lock{mutex_};
        records_.push_back(ContactRecord{.body1 = pair.GetBody1ID().GetIndexAndSequenceNumber(),
                                         .body2 = pair.GetBody2ID().GetIndexAndSequenceNumber(),
                                         .added = false,
                                         .point = JPH::RVec3::sZero(),
                                         .normal = JPH::Vec3::sZero()});
    }

    std::vector<ContactRecord> take() {
        const std::scoped_lock lock{mutex_};
        return std::exchange(records_, {});
    }

private:
    std::mutex mutex_;
    std::vector<ContactRecord> records_;
};

// ---------------------------------------------------------------------------------------------
// Umrechnung.

JPH::RVec3 to_jolt(math::DVec3 v) noexcept {
    return JPH::RVec3{v.x, v.y, v.z};
}
JPH::Vec3 to_jolt(math::Vec3 v) noexcept {
    return JPH::Vec3{v.x, v.y, v.z};
}
JPH::Quat to_jolt(math::Quat q) noexcept {
    return JPH::Quat{q.x, q.y, q.z, q.w};
}
math::DVec3 from_jolt(JPH::RVec3Arg v) noexcept {
    return math::DVec3{static_cast<f64>(v.GetX()), static_cast<f64>(v.GetY()),
                       static_cast<f64>(v.GetZ())};
}
math::Vec3 from_jolt(JPH::Vec3Arg v) noexcept {
    return math::Vec3{v.GetX(), v.GetY(), v.GetZ()};
}
math::Quat from_jolt(JPH::QuatArg q) noexcept {
    return math::Quat{q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}

bool same(const math::Vec3& a, const math::Vec3& b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
bool same(const world::Transform& a, const world::Transform& b) noexcept {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.rotation.x == b.rotation.x &&
           a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
           a.rotation.w == b.rotation.w && same(a.scale, b.scale);
}

// Alles an RigidBody außer den Geschwindigkeiten: Ändert es sich, entsteht der Körper neu.
bool same_settings(const RigidBody& a, const RigidBody& b) noexcept {
    return a.motion == b.motion && a.layer == b.layer && a.mass == b.mass &&
           a.friction == b.friction && a.restitution == b.restitution &&
           a.linear_damping == b.linear_damping && a.angular_damping == b.angular_damping &&
           a.gravity_factor == b.gravity_factor && a.allow_sleep == b.allow_sleep;
}
bool same_collider(const Collider& a, const Collider& b) noexcept {
    return a.shape == b.shape && a.radius == b.radius && same(a.half_extents, b.half_extents) &&
           a.half_height == b.half_height && same(a.offset, b.offset) && a.model == b.model &&
           a.mesh == b.mesh && a.sensor == b.sensor;
}

f32 max_abs(f32 a, f32 b) noexcept {
    return std::max(std::abs(a), std::abs(b));
}

JPH::Ref<JPH::Shape> created(JPH::ShapeSettings::ShapeResult result, std::string_view what) {
    if (result.HasError()) {
        log::warn("Physik: Form {} lässt sich nicht bauen: {}", what,
                  std::string_view{result.GetError().c_str()});
        return nullptr;
    }
    return result.Get();
}

// Grundformen (ohne Modell), skaliert.
JPH::Ref<JPH::Shape> primitive_shape(ShapeType type, f32 radius, math::Vec3 half_extents,
                                     f32 half_height, math::Vec3 scale) {
    constexpr f32 min_size = 0.001f;
    switch (type) {
    case ShapeType::Sphere: {
        const f32 r = std::max(
            radius * std::max({std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)}), min_size);
        JPH::SphereShapeSettings settings{r};
        return created(settings.Create(), "Kugel");
    }
    case ShapeType::Box: {
        const JPH::Vec3 half{std::max(half_extents.x * std::abs(scale.x), min_size),
                             std::max(half_extents.y * std::abs(scale.y), min_size),
                             std::max(half_extents.z * std::abs(scale.z), min_size)};
        // Die abgerundeten Kanten dürfen nicht größer als der Quader sein.
        const f32 convex = std::min(JPH::cDefaultConvexRadius, half.ReduceMin() * 0.5f);
        JPH::BoxShapeSettings settings{half, convex};
        return created(settings.Create(), "Quader");
    }
    case ShapeType::Capsule: {
        const f32 r = std::max(radius * max_abs(scale.x, scale.z), min_size);
        const f32 h = half_height * std::abs(scale.y);
        if (h <= 0.0f) {
            JPH::SphereShapeSettings settings{r};
            return created(settings.Create(), "Kapsel");
        }
        JPH::CapsuleShapeSettings settings{h, r};
        return created(settings.Create(), "Kapsel");
    }
    case ShapeType::ConvexHull:
    case ShapeType::Mesh: break;
    }
    return nullptr;
}

// Eintrag der Physik-Welt für eine Entity mit Körper.
struct BodyRecord {
    JPH::BodyID id;
    // Werte beim Anlegen bzw. zuletzt geschrieben; Abweichungen in der Welt sind Änderungen von
    // außen.
    RigidBody body;
    Collider collider;
    world::Transform transform;
    // Lage der Physik ist die Weltlage; mit Elternteil wird umgerechnet.
    bool has_parent = false;
};

struct ShapeKey {
    u64 model = 0;
    u32 mesh = 0;
    ShapeType type = ShapeType::ConvexHull;
    u32 scale_x = 0;
    u32 scale_y = 0;
    u32 scale_z = 0;
    friend bool operator<(const ShapeKey& a, const ShapeKey& b) noexcept {
        return std::tie(a.model, a.mesh, a.type, a.scale_x, a.scale_y, a.scale_z) <
               std::tie(b.model, b.mesh, b.type, b.scale_x, b.scale_y, b.scale_z);
    }
};

} // namespace

// -------------------------------------------------------------------------------------------------

struct PhysicsWorld::Impl {
    world::World* world;
    jobs::JobSystem* jobs;
    PhysicsSettings settings;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JobAdapter> job_adapter;
    BroadPhaseLayers broad_layers;
    ObjectVsBroadPhase object_vs_broad;
    ObjectPairs object_pairs;
    ContactCollector contacts;
    std::unique_ptr<JPH::PhysicsSystem> system;

    world::ComponentId transform_id{};
    world::ComponentId body_id{};
    world::ComponentId collider_id{};
    world::ComponentId interpolated_id{};

    // Nach Kennung sortiert: Abgleich und Rückschrieb laufen immer in derselben Reihenfolge.
    std::map<u64, BodyRecord> bodies;
    // Kennung des Körpers (Index und Folgenummer) zu Entity, für Kontakte.
    std::unordered_map<JPH::uint32, u64> entity_of_body;
    std::map<ShapeKey, JPH::Ref<JPH::Shape>> shape_cache;
    ShapeSource shape_source;
    // Paare in Berührung (a < b). Schlafen beide ein, meldet Jolt den Kontakt als gelöst; das Paar
    // bleibt dann als ruhend (true) stehen, bis einer aufwacht und die Berührung prüfbar ist.
    std::map<std::pair<u64, u64>, bool> touching;
    events::EventBus* bus = nullptr;
    StepStats stats;
    // Einmal pro Entity warnen statt in jedem Schritt.
    std::unordered_map<u64, bool> warned;

    Impl(world::World& w, jobs::JobSystem& j, PhysicsSettings s)
        : world{&w}, jobs{&j}, settings{s} {
        ensure_runtime();
        temp =
            std::make_unique<JPH::TempAllocatorImpl>(static_cast<JPH::uint>(settings.temp_memory));
        job_adapter =
            std::make_unique<JobAdapter>(j, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
        create_system();
        transform_id = w.component<world::Transform>();
        body_id = w.component<RigidBody>();
        collider_id = w.component<Collider>();
        interpolated_id = w.component<world::Interpolated>();
    }

    ~Impl() {
        remove_all();
        job_adapter->drain();
    }

    // Neue, leere Jolt-Welt. Jolt ist nur bitgleich, wenn auch der innere Zustand gleich ist
    // (Kennungen der Körper, Zwischenspeicher der Kontakte); nach dem Verwerfen aller Körper
    // beginnt deshalb eine frische Welt.
    void create_system() {
        system = std::make_unique<JPH::PhysicsSystem>();
        system->Init(settings.max_bodies, 0, settings.max_body_pairs,
                     settings.max_contact_constraints, broad_layers, object_vs_broad, object_pairs);
        system->SetGravity(to_jolt(settings.gravity));
        system->SetContactListener(&contacts);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    JPH::BodyInterface& body_interface() noexcept {
        return system->GetBodyInterfaceNoLock();
    }

    void warn_once(u64 entity, std::string_view message) {
        if (!warned[entity]) {
            warned[entity] = true;
            log::warn("Physik: Entity {}: {}", entity, message);
        }
    }

    // Weltlage einer Entity (Position, Drehung, Skalierung).
    math::Decomposed world_pose(u64 entity, const world::Transform& transform, bool& has_parent) {
        has_parent = world->parent(entity) != 0;
        if (!has_parent) {
            return math::Decomposed{.translation = transform.position,
                                    .rotation = transform.rotation,
                                    .scale = transform.scale};
        }
        return math::decompose(world::world_affine(*world, entity));
    }

    JPH::Ref<JPH::Shape> model_shape(u64 entity, const Collider& collider, ShapeType type,
                                     math::Vec3 scale) {
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

    JPH::Ref<JPH::Shape> make_shape(u64 entity, const RigidBody& body, const Collider& collider,
                                    math::Vec3 scale) {
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
            shape = primitive_shape(type, collider.radius, collider.half_extents,
                                    collider.half_height, scale);
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
    bool create_body(u64 entity, const RigidBody& body, const Collider& collider,
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
        body_interface().AddBody(id, active ? JPH::EActivation::Activate
                                            : JPH::EActivation::DontActivate);
        bodies[entity] = BodyRecord{.id = id,
                                    .body = body,
                                    .collider = collider,
                                    .transform = transform,
                                    .has_parent = has_parent};
        entity_of_body[id.GetIndexAndSequenceNumber()] = entity;
        return true;
    }

    void destroy_body(std::map<u64, BodyRecord>::iterator it, std::vector<ContactEnded>& ended,
                      std::vector<TriggerLeft>& left) {
        const u64 entity = it->first;
        const JPH::BodyID id = it->second.id;
        body_interface().RemoveBody(id);
        body_interface().DestroyBody(id);
        entity_of_body.erase(id.GetIndexAndSequenceNumber());
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

    [[nodiscard]] bool is_sensor_entity(u64 entity) const {
        const auto found = bodies.find(entity);
        if (found == bodies.end()) {
            return false;
        }
        return found->second.collider.sensor || found->second.body.layer == PhysicsLayer::Sensor;
    }

    void emit_end(u64 a, u64 b, std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left) {
        if (is_sensor_entity(a)) {
            left.push_back(TriggerLeft{.sensor = a, .other = b});
        } else if (is_sensor_entity(b)) {
            left.push_back(TriggerLeft{.sensor = b, .other = a});
        } else {
            ended.push_back(ContactEnded{.a = a, .b = b});
        }
    }

    void remove_all() {
        std::vector<ContactEnded> ended;
        std::vector<TriggerLeft> left;
        while (!bodies.empty()) {
            destroy_body(bodies.begin(), ended, left);
        }
        touching.clear();
        (void)contacts.take();
    }

    void clear() {
        remove_all();
        create_system();
        shape_cache.clear();
        warned.clear();
    }

    void sync(std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left, f32 dt) {
        NGINE_PROFILE_ZONE("PhysicsWorld::sync");
        std::vector<u64> current;
        world->each<const RigidBody, const Collider, const world::Transform>(
            [&](world::Entity entity, const RigidBody&, const Collider&, const world::Transform&) {
                current.push_back(entity.id());
            });
        std::ranges::sort(current);

        // Weggefallene Körper zuerst, in der Reihenfolge der Kennungen.
        for (auto it = bodies.begin(); it != bodies.end();) {
            if (!std::ranges::binary_search(current, it->first)) {
                auto next = std::next(it);
                destroy_body(it, ended, left);
                it = next;
            } else {
                ++it;
            }
        }

        // Interpolation für bewegliche Körper: Strukturänderung vor allen Zeigern in die Welt.
        for (const u64 entity : current) {
            if (bodies.contains(entity) || world->has(entity, interpolated_id)) {
                continue;
            }
            const auto* body = static_cast<const RigidBody*>(world->get(entity, body_id));
            if (body->motion != MotionType::Static) {
                (void)world->ensure(entity, interpolated_id);
                world->modified(entity, interpolated_id);
            }
        }

        usize added = 0;
        for (const u64 entity : current) {
            const auto* body = static_cast<const RigidBody*>(world->get(entity, body_id));
            const auto* collider = static_cast<const Collider*>(world->get(entity, collider_id));
            const auto* transform =
                static_cast<const world::Transform*>(world->get(entity, transform_id));
            auto found = bodies.find(entity);
            if (found != bodies.end()) {
                BodyRecord& record = found->second;
                const bool rebuild = !same_settings(record.body, *body) ||
                                     !same_collider(record.collider, *collider) ||
                                     !same(record.transform.scale, transform->scale);
                if (rebuild) {
                    destroy_body(found, ended, left);
                } else {
                    update_body(entity, record, *body, *transform, dt);
                    continue;
                }
            }
            if (create_body(entity, *body, *collider, *transform)) {
                ++added;
            }
        }
        if (added > 64) {
            // Viele neue Körper auf einmal: Breitenphase neu ordnen, sonst sind die ersten Schritte
            // langsam.
            system->OptimizeBroadPhase();
        }
    }

    // Änderungen von außen an einem bestehenden Körper.
    void update_body(u64 entity, BodyRecord& record, const RigidBody& body,
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

    void write_back() {
        NGINE_PROFILE_ZONE("PhysicsWorld::write_back");
        JPH::BodyInterface& bodies_interface = body_interface();
        u32 active = 0;
        for (auto& [entity, record] : bodies) {
            if (record.body.motion != MotionType::Dynamic) {
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
                const math::Affine global = math::Affine::from(
                    from_jolt(position), from_jolt(rotation), {1.0f, 1.0f, 1.0f});
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

    void publish(std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left) {
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
                if (!change.has_point ||
                    std::tie(point.x, point.y, point.z) <
                        std::tie(change.point.x, change.point.y, change.point.z)) {
                    change.point = point;
                    change.normal = normal;
                    change.has_point = true;
                }
            }
        }

        JPH::BodyInterface& bodies_interface = body_interface();
        const auto body_of = [&](u64 entity) { return bodies.at(entity).id; };
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
                        entered.push_back(
                            TriggerEntered{.sensor = pair.first, .other = pair.second});
                    } else if (is_sensor_entity(pair.second)) {
                        entered.push_back(
                            TriggerEntered{.sensor = pair.second, .other = pair.first});
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
    }

    JPH::Ref<JPH::Shape> query_shape(const QueryShape& shape) const {
        return primitive_shape(shape.shape, shape.radius, shape.half_extents, shape.half_height,
                               {1.0f, 1.0f, 1.0f});
    }

    [[nodiscard]] u64 entity_of(JPH::BodyID id) const {
        const auto found = entity_of_body.find(id.GetIndexAndSequenceNumber());
        return found != entity_of_body.end() ? found->second : 0;
    }

    RayHit ray_hit(const JPH::RRayCast& ray, const JPH::RayCastResult& result,
                   f32 max_distance) const {
        RayHit hit;
        hit.entity = entity_of(result.mBodyID);
        const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
        hit.point = from_jolt(point);
        hit.distance = result.mFraction * max_distance;
        const JPH::BodyLockRead lock{system->GetBodyLockInterfaceNoLock(), result.mBodyID};
        if (lock.Succeeded()) {
            hit.normal =
                from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
        }
        return hit;
    }
};

// -------------------------------------------------------------------------------------------------

PhysicsWorld::PhysicsWorld(world::World& world, jobs::JobSystem& jobs, PhysicsSettings settings)
    : impl_{std::make_unique<Impl>(world, jobs, settings)} {}

PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::set_shape_source(ShapeSource source) {
    impl_->shape_source = std::move(source);
    impl_->shape_cache.clear();
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
    const auto synced = clock::now();
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
}

void PhysicsWorld::clear() {
    impl_->clear();
}

std::optional<RayHit> PhysicsWorld::raycast(math::DVec3 origin, math::Vec3 direction,
                                            f32 max_distance, QueryFilter filter) const {
    if (math::length_squared(direction) == 0.0f || max_distance <= 0.0f) {
        return std::nullopt;
    }
    const JPH::RRayCast ray{to_jolt(origin), to_jolt(math::normalize(direction) * max_distance)};
    JPH::RayCastResult result;
    const QueryBroadFilter broad{filter.layers};
    const QueryObjectFilter object{filter.layers};
    const QueryBodyFilter body{filter.ignore, filter.sensors};
    if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, result, broad, object, body)) {
        return std::nullopt;
    }
    return impl_->ray_hit(ray, result, max_distance);
}

std::vector<RayHit> PhysicsWorld::raycast_all(math::DVec3 origin, math::Vec3 direction,
                                              f32 max_distance, QueryFilter filter) const {
    std::vector<RayHit> hits;
    if (math::length_squared(direction) == 0.0f || max_distance <= 0.0f) {
        return hits;
    }
    const JPH::RRayCast ray{to_jolt(origin), to_jolt(math::normalize(direction) * max_distance)};
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const QueryBroadFilter broad{filter.layers};
    const QueryObjectFilter object{filter.layers};
    const QueryBodyFilter body{filter.ignore, filter.sensors};
    impl_->system->GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings{}, collector, broad,
                                                 object, body);
    collector.Sort();
    hits.reserve(collector.mHits.size());
    for (const JPH::RayCastResult& result : collector.mHits) {
        hits.push_back(impl_->ray_hit(ray, result, max_distance));
    }
    return hits;
}

std::optional<ShapeHit> PhysicsWorld::shape_cast(const QueryShape& shape, math::DVec3 origin,
                                                 math::Quat rotation, math::Vec3 direction,
                                                 f32 max_distance, QueryFilter filter) const {
    if (math::length_squared(direction) == 0.0f || max_distance <= 0.0f) {
        return std::nullopt;
    }
    const JPH::Ref<JPH::Shape> cast_shape = impl_->query_shape(shape);
    if (cast_shape == nullptr) {
        return std::nullopt;
    }
    const JPH::Vec3 movement = to_jolt(math::normalize(direction) * max_distance);
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        cast_shape.GetPtr(), JPH::Vec3::sOne(),
        JPH::RMat44::sRotationTranslation(to_jolt(math::normalize(rotation)), to_jolt(origin)),
        movement);
    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint = true;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const QueryBroadFilter broad{filter.layers};
    const QueryObjectFilter object{filter.layers};
    const QueryBodyFilter body{filter.ignore, filter.sensors};
    impl_->system->GetNarrowPhaseQuery().CastShape(cast, settings, to_jolt(origin), collector,
                                                   broad, object, body);
    if (!collector.HadHit()) {
        return std::nullopt;
    }
    const JPH::ShapeCastResult& result = collector.mHit;
    ShapeHit hit;
    hit.entity = impl_->entity_of(result.mBodyID2);
    hit.point = from_jolt(to_jolt(origin) + result.mContactPointOn2);
    const JPH::Vec3 axis = result.mPenetrationAxis;
    hit.normal =
        axis.LengthSq() > 0.0f ? from_jolt(-axis.Normalized()) : -math::normalize(direction);
    hit.distance = result.mFraction * max_distance;
    return hit;
}

std::vector<u64> PhysicsWorld::overlap(const QueryShape& shape, math::DVec3 position,
                                       math::Quat rotation, QueryFilter filter) const {
    std::vector<u64> result;
    const JPH::Ref<JPH::Shape> query = impl_->query_shape(shape);
    if (query == nullptr) {
        return result;
    }
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const QueryBroadFilter broad{filter.layers};
    const QueryObjectFilter object{filter.layers};
    const QueryBodyFilter body{filter.ignore, filter.sensors};
    const JPH::RMat44 transform =
        JPH::RMat44::sRotationTranslation(to_jolt(math::normalize(rotation)), to_jolt(position));
    impl_->system->GetNarrowPhaseQuery().CollideShape(
        query.GetPtr(), JPH::Vec3::sOne(), transform.PreTranslated(query->GetCenterOfMass()),
        JPH::CollideShapeSettings{}, to_jolt(position), collector, broad, object, body);
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        const u64 entity = impl_->entity_of(hit.mBodyID2);
        if (entity != 0) {
            result.push_back(entity);
        }
    }
    std::ranges::sort(result);
    const auto [first, last] = std::ranges::unique(result);
    result.erase(first, last);
    return result;
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
