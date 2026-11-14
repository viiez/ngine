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
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

// Sucht die Definitionen, in denen sich Bibliothek und Adapter unterscheiden: Die Bibliothek
// vergleicht nur die ganze Kennung, also probieren wir jedes Merkmal einzeln und paarweise.
std::string version_mismatch() {
    constexpr std::array<const char*, 11> names{"JPH_DOUBLE_PRECISION",
                                                "JPH_CROSS_PLATFORM_DETERMINISTIC",
                                                "JPH_FLOATING_POINT_EXCEPTIONS_ENABLED",
                                                "JPH_PROFILE_ENABLED",
                                                "JPH_EXTERNAL_PROFILE",
                                                "JPH_DEBUG_RENDERER",
                                                "JPH_DISABLE_TEMP_ALLOCATOR",
                                                "JPH_DISABLE_CUSTOM_ALLOCATOR",
                                                "JPH_OBJECT_LAYER_BITS",
                                                "JPH_ENABLE_ASSERTS",
                                                "JPH_OBJECT_STREAM"};
    using JPH::uint64; // JPH_VERSION_ID nennt den Typ ohne Namensraum.
    const uint64 own = JPH_VERSION_ID;
    const auto bit = [](usize index) { return JPH::uint64{1} << (24 + index); };
    const auto name = [&](usize index) {
        return std::format("{} im Adapter {}", names[index],
                           (own & bit(index)) != 0 ? "an" : "aus");
    };
    for (usize i = 0; i < names.size(); ++i) {
        if (JPH::VerifyJoltVersionIDInternal(own ^ bit(i))) {
            return std::format("abweichend: {}", name(i));
        }
    }
    for (usize i = 0; i < names.size(); ++i) {
        for (usize j = i + 1; j < names.size(); ++j) {
            if (JPH::VerifyJoltVersionIDInternal(own ^ bit(i) ^ bit(j))) {
                return std::format("abweichend: {}, {}", name(i), name(j));
            }
        }
    }
    return std::format("Kennung des Adapters {:#x}, andere Version oder mehr als zwei Merkmale",
                       own);
}

class JoltRuntime {
public:
    JoltRuntime() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = &trace;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = &assert_failed;
#endif
        // Bricht ab, wenn Bibliothek und Adapter mit anderen Schaltern gebaut sind (z. B. ohne
        // doppelte Genauigkeit), und nennt die abweichenden Definitionen.
        if (!JPH::VerifyJoltVersionID()) {
            log::fatal("Jolt: Bibliothek und Adapter passen nicht zusammen ({})",
                       version_mismatch());
        }
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
//
// Unter ThreadSanitizer rechnet Jolt nur im wartenden Thread: Die Bibliothek aus vcpkg ist nicht
// instrumentiert, ThreadSanitizer sähe ihre Synchronisation nicht und meldete Wettläufe, die es
// nicht gibt (TD-47).
#if defined(__SANITIZE_THREAD__)
constexpr bool jolt_on_workers = false;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool jolt_on_workers = false;
#else
constexpr bool jolt_on_workers = true;
#endif
#else
constexpr bool jolt_on_workers = true;
#endif

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
        return jolt_on_workers ? static_cast<int>(jobs_->worker_count()) + 1 : 1;
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
        if constexpr (!jolt_on_workers) {
            // Die Barriere rechnet den Job, sobald jemand auf sie wartet.
            return;
        }
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

// ---------------------------------------------------------------------------------------------
// Kollision des Terrains (R4.3): eine Form pro Chunk der Stufe 0 aus dem Netz, das der Mesher
// ohnehin baut.

// Eine Form im Bau. Der Job schreibt shape und setzt done; der Hauptthread liest erst danach.
struct TerrainBuild {
    u64 version = 0;
    std::atomic<bool> done{false};
    JPH::Ref<JPH::Shape> shape;
    u32 triangles = 0;
    f64 ms = 0.0;
};

struct TerrainChunk {
    JPH::BodyID body;
    // Fassung des Netzes, aus der die eingesetzte Form stammt (0: keine).
    u64 version = 0;
    u32 triangles = 0;
    usize bytes = 0;
    std::shared_ptr<TerrainBuild> build;
    // Schritte, seit nichts die Form mehr braucht.
    u32 unused = 0;
};

struct TerrainRecord {
    // TerrainData::id; ändert er sich, entsteht das Terrain neu und alle Formen fallen weg.
    u64 data_id = 0;
    math::DVec3 base{0.0, 0.0, 0.0};
    std::map<voxel::ChunkCoord, TerrainChunk> chunks;
};

// Baut die Form eines Chunks aus seinem Netz (Positionen in Metern relativ zur Ecke). nullptr bei
// einem Netz ohne brauchbare Dreiecke.
JPH::Ref<JPH::Shape> terrain_shape(JPH::VertexList vertices, JPH::IndexedTriangleList triangles) {
    if (triangles.empty()) {
        return nullptr;
    }
    JPH::MeshShapeSettings settings{std::move(vertices), std::move(triangles)};
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        return nullptr;
    }
    return result.Get();
}

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
    // Kollision der Terrains nach Entity des Terrains.
    std::map<u64, TerrainRecord> terrains;
    // Körper der Chunks zu Entity des Terrains (für Abfragen; Kontakte melden sie nicht).
    std::unordered_map<JPH::uint32, u64> terrain_of_body;
    // Laufende Jobs, die Formen bauen.
    jobs::Counter terrain_jobs;
    // Eingefrorene Körper (ohne Form des Terrains unter sich), nach Kennung.
    std::map<u64, bool> frozen;
    world::ComponentId terrain_state_id{};
    world::ComponentId world_transform_id{};
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
        terrain_state_id = w.component<world::TerrainState>();
        world_transform_id = w.component<world::WorldTransform>();
    }

    ~Impl() {
        jobs->wait(terrain_jobs);
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

    void clear() {
        jobs->wait(terrain_jobs);
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

    void write_back() {
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

    // --- Kollision des Terrains (R4.3) -----------------------------------------------------------

    void remove_chunk_body(TerrainChunk& chunk) {
        if (chunk.body.IsInvalid()) {
            return;
        }
        terrain_of_body.erase(chunk.body.GetIndexAndSequenceNumber());
        body_interface().RemoveBody(chunk.body);
        body_interface().DestroyBody(chunk.body);
        chunk.body = {};
    }

    // Lage aller Dinge, die Formen brauchen: dynamische Körper und Beobachter (Spielfiguren
    // folgen mit ihnen).
    std::vector<math::DVec3> terrain_anchors() {
        std::vector<math::DVec3> anchors;
        JPH::BodyInterface& bodies_interface = body_interface();
        for (const auto& [entity, record] : bodies) {
            if (record.body.motion == MotionType::Dynamic) {
                anchors.push_back(from_jolt(bodies_interface.GetPosition(record.id)));
            }
        }
        world->each<const world::TerrainObserver, const world::WorldTransform>(
            [&](world::Entity, const world::TerrainObserver& observer,
                const world::WorldTransform& transform) {
                if (observer.active) {
                    anchors.push_back(transform.position());
                }
            });
        return anchors;
    }

    // Chunks der Stufe 0 eines Terrains, die eine Form brauchen: mit Netz und im Radius um einen
    // Anker. Sortiert.
    std::vector<voxel::ChunkCoord> needed_chunks(const world::TerrainData& data, math::DVec3 base,
                                                 std::span<const math::DVec3> anchors) const {
        const f64 radius = static_cast<f64>(std::max(settings.terrain_radius, 0.0f));
        const auto reach = static_cast<i32>(std::ceil(radius / voxel::chunk_size));
        std::unordered_set<voxel::ChunkCoord, voxel::ChunkCoordHash> centers;
        for (const math::DVec3& anchor : anchors) {
            const math::DVec3 local = (anchor - base) * (1.0 / voxel::voxel_size);
            centers.insert(voxel::chunk_of(static_cast<i64>(std::floor(local.x)),
                                           static_cast<i64>(std::floor(local.y)),
                                           static_cast<i64>(std::floor(local.z))));
        }
        std::unordered_set<voxel::ChunkCoord, voxel::ChunkCoordHash> needed;
        for (const voxel::ChunkCoord center : centers) {
            for (i32 dz = -reach; dz <= reach; ++dz) {
                for (i32 dy = -reach; dy <= reach; ++dy) {
                    for (i32 dx = -reach; dx <= reach; ++dx) {
                        const voxel::ChunkCoord coord{center.x + dx, center.y + dy, center.z + dz,
                                                      0};
                        if (needed.contains(coord) || !data.meshes.contains(coord)) {
                            continue;
                        }
                        needed.insert(coord);
                    }
                }
            }
        }
        // Genauer: nur Chunks, deren Würfel näher als radius an einem Anker liegt. Die Mitten der
        // Würfel reichen als Maß; reach deckt die Nachbarn ab.
        std::vector<voxel::ChunkCoord> result;
        result.reserve(needed.size());
        const f64 half = voxel::chunk_size * 0.5;
        const f64 limit = radius + half * 1.7320508;
        for (const voxel::ChunkCoord coord : needed) {
            const math::DVec3 center =
                base + voxel::chunk_position(coord) + math::DVec3{half, half, half};
            for (const math::DVec3& anchor : anchors) {
                const math::DVec3 d = center - anchor;
                if (d.x * d.x + d.y * d.y + d.z * d.z <= limit * limit) {
                    result.push_back(coord);
                    break;
                }
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    // Startet einen Job, der die Form aus dem Netz baut.
    void start_build(TerrainChunk& chunk, const world::ChunkMesh& mesh) {
        auto build = std::make_shared<TerrainBuild>();
        build->version = mesh.version;
        JPH::VertexList vertices;
        vertices.reserve(mesh.mesh.vertices.size());
        const auto scale = static_cast<f32>(voxel::voxel_size);
        for (const voxel::Vertex& v : mesh.mesh.vertices) {
            vertices.push_back(JPH::Float3{v.px * scale, v.py * scale, v.pz * scale});
        }
        JPH::IndexedTriangleList triangles;
        triangles.reserve(mesh.mesh.indices.size() / 3);
        for (usize i = 0; i + 2 < mesh.mesh.indices.size(); i += 3) {
            triangles.push_back(JPH::IndexedTriangle{mesh.mesh.indices[i], mesh.mesh.indices[i + 1],
                                                     mesh.mesh.indices[i + 2], 0});
        }
        chunk.build = build;
        auto work = [build, vertices = std::move(vertices),
                     triangles = std::move(triangles)]() mutable {
            const auto start = std::chrono::steady_clock::now();
            build->triangles = static_cast<u32>(triangles.size());
            build->shape = terrain_shape(std::move(vertices), std::move(triangles));
            build->ms =
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            build->done.store(true, std::memory_order_release);
        };
        if constexpr (jolt_on_workers) {
            jobs->run(std::move(work), &terrain_jobs);
        } else {
            // Unter ThreadSanitizer baut Jolt nur im Hauptthread (TD-47).
            work();
        }
    }

    // Setzt eine fertige Form ein; liefert, ob sich die Kollision geändert hat.
    bool finish_build(u64 terrain_entity, const TerrainRecord& terrain, voxel::ChunkCoord coord,
                      TerrainChunk& chunk, f64& build_ms, u32& built) {
        std::shared_ptr<TerrainBuild> build = std::move(chunk.build);
        chunk.build.reset();
        built += 1;
        build_ms += build->ms;
        JPH::BodyInterface& bodies_interface = body_interface();
        if (build->shape == nullptr) {
            // Netz ohne Dreiecke: keine Form.
            remove_chunk_body(chunk);
            chunk.version = build->version;
            chunk.triangles = 0;
            chunk.bytes = 0;
            return true;
        }
        chunk.version = build->version;
        chunk.triangles = build->triangles;
        chunk.bytes = build->shape->GetStats().mSizeBytes;
        if (chunk.body.IsInvalid()) {
            JPH::BodyCreationSettings creation{build->shape.GetPtr(),
                                               to_jolt(terrain.base + voxel::chunk_position(coord)),
                                               JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                               object_layer(PhysicsLayer::Static)};
            creation.mUserData = terrain_entity;
            JPH::Body* body = bodies_interface.CreateBody(creation);
            if (body == nullptr) {
                warn_once(terrain_entity, "kein Platz mehr für Formen des Terrains");
                return false;
            }
            chunk.body = body->GetID();
            bodies_interface.AddBody(chunk.body, JPH::EActivation::DontActivate);
            terrain_of_body[chunk.body.GetIndexAndSequenceNumber()] = terrain_entity;
        } else {
            bodies_interface.SetShape(chunk.body, build->shape.GetPtr(), false,
                                      JPH::EActivation::DontActivate);
        }
        // Körper in der Nähe wecken: Liegt eine Kiste auf weggegrabenem Boden, fällt sie.
        const math::DVec3 corner = terrain.base + voxel::chunk_position(coord);
        constexpr f64 margin = 1.0;
        const auto low = math::to_f32(corner - math::DVec3{margin, margin, margin});
        const auto high = math::to_f32(corner + math::DVec3{voxel::chunk_size + margin,
                                                            voxel::chunk_size + margin,
                                                            voxel::chunk_size + margin});
        const QueryBroadFilter broad{all_layers};
        const QueryObjectFilter object{all_layers};
        bodies_interface.ActivateBodiesInAABox(
            JPH::AABox{JPH::Vec3{low.x, low.y, low.z}, JPH::Vec3{high.x, high.y, high.z}}, broad,
            object);
        return true;
    }

    // Hält die Formen aller Terrains aktuell.
    void sync_terrains() {
        NGINE_PROFILE_ZONE("PhysicsWorld::sync_terrains");
        struct Live {
            u64 entity;
            std::shared_ptr<world::TerrainData> data;
            math::DVec3 base;
        };
        std::vector<Live> live;
        world->each<const world::TerrainState, const world::WorldTransform>(
            [&](world::Entity entity, const world::TerrainState& state,
                const world::WorldTransform& transform) {
                if (state.data) {
                    live.push_back(Live{entity.id(), state.data, transform.position()});
                }
            });
        std::ranges::sort(live, {}, &Live::entity);

        // Weggefallene Terrains.
        for (auto it = terrains.begin(); it != terrains.end();) {
            const bool alive =
                std::ranges::any_of(live, [&](const Live& l) { return l.entity == it->first; });
            if (!alive) {
                for (auto& [coord, chunk] : it->second.chunks) {
                    remove_chunk_body(chunk);
                }
                it = terrains.erase(it);
            } else {
                ++it;
            }
        }
        if (live.empty()) {
            stats.terrain_chunks = 0;
            stats.terrain_building = 0;
            stats.terrain_built = 0;
            stats.terrain_bytes = 0;
            return;
        }

        const std::vector<math::DVec3> anchors = terrain_anchors();
        f64 build_ms = 0.0;
        u32 built = 0;
        for (const Live& l : live) {
            TerrainRecord& terrain = terrains[l.entity];
            if (terrain.data_id != l.data->id) {
                for (auto& [coord, chunk] : terrain.chunks) {
                    remove_chunk_body(chunk);
                }
                terrain.chunks.clear();
                terrain.data_id = l.data->id;
                terrain.base = l.base;
            }
            if (!(terrain.base == l.base)) {
                // Das Terrain wurde verschoben: alle Formen mit.
                terrain.base = l.base;
                for (auto& [coord, chunk] : terrain.chunks) {
                    if (!chunk.body.IsInvalid()) {
                        body_interface().SetPosition(chunk.body,
                                                     to_jolt(l.base + voxel::chunk_position(coord)),
                                                     JPH::EActivation::DontActivate);
                    }
                }
            }
            const std::vector<voxel::ChunkCoord> needed = needed_chunks(*l.data, l.base, anchors);
            for (const voxel::ChunkCoord coord : needed) {
                TerrainChunk& chunk = terrain.chunks[coord];
                chunk.unused = 0;
                const world::ChunkMesh& mesh = l.data->meshes.at(coord);
                const bool current = chunk.version == mesh.version;
                const bool building = chunk.build && chunk.build->version == mesh.version;
                if (!current && !building) {
                    start_build(chunk, mesh);
                }
            }
            // Formen ohne Bedarf oder ohne Netz fallen nach einer Weile weg.
            for (auto it = terrain.chunks.begin(); it != terrain.chunks.end();) {
                TerrainChunk& chunk = it->second;
                const bool has_mesh = l.data->meshes.contains(it->first);
                if (!std::binary_search(needed.begin(), needed.end(), it->first)) {
                    ++chunk.unused;
                }
                if (!has_mesh || chunk.unused > settings.terrain_keep_steps) {
                    if (!chunk.build) {
                        remove_chunk_body(chunk);
                        it = terrain.chunks.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }

        if (settings.wait_for_terrain) {
            jobs->wait(terrain_jobs);
        }
        // Fertige Formen einsetzen, in der Reihenfolge der Terrains und Chunks.
        u32 building = 0;
        u32 chunks_with_shape = 0;
        usize bytes = 0;
        for (auto& [entity, terrain] : terrains) {
            for (auto it = terrain.chunks.begin(); it != terrain.chunks.end();) {
                TerrainChunk& chunk = it->second;
                if (chunk.build && chunk.build->done.load(std::memory_order_acquire)) {
                    (void)finish_build(entity, terrain, it->first, chunk, build_ms, built);
                    if (chunk.body.IsInvalid() && chunk.unused > settings.terrain_keep_steps) {
                        it = terrain.chunks.erase(it);
                        continue;
                    }
                }
                if (chunk.build) {
                    ++building;
                }
                if (!chunk.body.IsInvalid()) {
                    ++chunks_with_shape;
                    bytes += chunk.bytes;
                }
                ++it;
            }
        }
        stats.terrain_chunks = chunks_with_shape;
        stats.terrain_building = building;
        stats.terrain_built = built;
        if (built > 0) {
            stats.terrain_build_ms = build_ms / static_cast<f64>(built);
        }
        stats.terrain_bytes = bytes;
    }

    // Ob unter einer Lage die Kollision des Terrains fehlt: Der Chunk der Stufe 0 an der Lage
    // oder einer der beiden darunter hat ein Netz, aber noch keine Form dazu, oder ein endloses
    // Terrain hat dort noch gar keine Chunks der Stufe 0 geladen.
    [[nodiscard]] bool missing_ground(math::DVec3 position) const {
        for (const auto& [entity, terrain] : terrains) {
            const world::Entity handle = world->entity(entity);
            const auto* state =
                static_cast<const world::TerrainState*>(world->get(entity, terrain_state_id));
            if (state == nullptr || !state->data || !handle) {
                continue;
            }
            const world::TerrainData& data = *state->data;
            const math::DVec3 local = (position - terrain.base) * (1.0 / voxel::voxel_size);
            const voxel::ChunkCoord at = voxel::chunk_of(static_cast<i64>(std::floor(local.x)),
                                                         static_cast<i64>(std::floor(local.y)),
                                                         static_cast<i64>(std::floor(local.z)));
            bool loaded = false;
            for (i32 dy = 0; dy <= 2; ++dy) {
                const voxel::ChunkCoord coord{at.x, at.y - dy, at.z, 0};
                if (data.chunks.find(coord) != nullptr) {
                    loaded = true;
                }
                const auto mesh = data.meshes.find(coord);
                if (mesh == data.meshes.end()) {
                    continue;
                }
                loaded = true;
                const auto chunk = terrain.chunks.find(coord);
                if (chunk == terrain.chunks.end() || chunk->second.version == 0) {
                    return true;
                }
            }
            if (!loaded && data.streamer) {
                return true;
            }
        }
        return false;
    }

    // Friert dynamische Körper ohne Boden ein und taut sie auf, sobald er da ist.
    void update_frozen() {
        NGINE_PROFILE_ZONE("PhysicsWorld::update_frozen");
        JPH::BodyInterface& bodies_interface = body_interface();
        for (auto& [entity, record] : bodies) {
            if (record.body.motion != MotionType::Dynamic) {
                continue;
            }
            const bool is_frozen = frozen.contains(entity);
            if (!is_frozen && !bodies_interface.IsActive(record.id)) {
                // Schlafende Körper bleiben liegen, bis jemand sie weckt.
                continue;
            }
            const bool missing = !terrains.empty() &&
                                 missing_ground(from_jolt(bodies_interface.GetPosition(record.id)));
            if (missing && !is_frozen) {
                bodies_interface.SetMotionType(record.id, JPH::EMotionType::Kinematic,
                                               JPH::EActivation::DontActivate);
                bodies_interface.SetLinearAndAngularVelocity(record.id, JPH::Vec3::sZero(),
                                                             JPH::Vec3::sZero());
                frozen[entity] = true;
            } else if (!missing && is_frozen) {
                bodies_interface.SetMotionType(record.id, JPH::EMotionType::Dynamic,
                                               JPH::EActivation::Activate);
                bodies_interface.SetLinearAndAngularVelocity(record.id,
                                                             to_jolt(record.body.linear_velocity),
                                                             to_jolt(record.body.angular_velocity));
                frozen.erase(entity);
            }
        }
        stats.frozen_bodies = static_cast<u32>(frozen.size());
    }

    JPH::Ref<JPH::Shape> query_shape(const QueryShape& shape) const {
        return primitive_shape(shape.shape, shape.radius, shape.half_extents, shape.half_height,
                               {1.0f, 1.0f, 1.0f});
    }

    [[nodiscard]] u64 entity_of(JPH::BodyID id) const {
        const JPH::uint32 key = id.GetIndexAndSequenceNumber();
        if (const auto found = entity_of_body.find(key); found != entity_of_body.end()) {
            return found->second;
        }
        const auto terrain = terrain_of_body.find(key);
        return terrain != terrain_of_body.end() ? terrain->second : 0;
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
    impl.sync_terrains();
    impl.update_frozen();
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
