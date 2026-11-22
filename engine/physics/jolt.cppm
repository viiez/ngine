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

// Gemeinsamer Teil des Adapters für Jolt (R5.1, TD-64): Laufzeit, Job-System, Ebenen und Filter,
// Materialien, Kontakte, Umrechnung, Einträge der Körper, Figuren und Chunks sowie
// PhysicsWorld::Impl selbst. Interne Partition: Nur die Dateien jolt_*.cpp sehen sie. Bis R4.6
// stand alles in einer Datei mit rund 2 260 Zeilen; die Methoden von Impl verteilen sich jetzt nach
// Aufgabe:
//
//   jolt_world.cpp       Körper abgleichen, Schritt, Rückschrieb, Kontakte, öffentliche
//   Schnittstelle jolt_characters.cpp  Spielfiguren (CharacterVirtual) jolt_terrain.cpp Kollision
//   des Terrains, eingefrorene Körper jolt_queries.cpp     Strahlen, Formen und Überlappung
module ngine.physics:jolt;

import ngine.core;
import ngine.math;
import ngine.jobs;
import ngine.profile;
import ngine.world;
import :components;
import :physics_world;

namespace ngine::physics {

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
// Seit R5.1 (TD-47) auch unter ThreadSanitizer auf den Workern: Das Preset linux-clang-tsan baut
// Jolt über das Triplet x64-linux-tsan mit ThreadSanitizer (cmake/vcpkg-triplets).

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

inline constexpr JPH::BroadPhaseLayer broad_static{0};
inline constexpr JPH::BroadPhaseLayer broad_moving{1};
inline constexpr JPH::uint broad_count = 2;

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
// Materialien des Terrains (R4.3): Jedes Dreieck einer Chunk-Form trägt die Nummer seines
// Voxel-Materials; Reibung und Rückprall kommen aus der Materialtabelle (world::TerrainMaterials).

class TerrainSurface final : public JPH::PhysicsMaterial {
public:
    explicit TerrainSurface(voxel::Material material) : material{material} {}
    [[nodiscard]] const char* GetDebugName() const override {
        return "Terrain";
    }
    voxel::Material material;
};

struct SurfaceValues {
    f32 friction = 0.7f;
    f32 restitution = 0.0f;
};
using SurfaceTable = std::array<SurfaceValues, world::max_terrain_materials>;

// Nummer des Voxel-Materials an einer Stelle einer Form, wenn sie zum Terrain gehört.
std::optional<voxel::Material> terrain_material(const JPH::Shape& shape, JPH::SubShapeID id) {
    const auto* surface = dynamic_cast<const TerrainSurface*>(shape.GetMaterial(id));
    if (surface == nullptr) {
        return std::nullopt;
    }
    return surface->material;
}

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
    // Tabelle der Oberflächen; ändert sich nur zwischen den Schritten.
    const SurfaceTable* surfaces = nullptr;

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override {
        apply_surface(body1, body2, manifold, settings);
        const JPH::RVec3 point = manifold.GetWorldSpaceContactPointOn1(0);
        const std::scoped_lock lock{mutex_};
        records_.push_back(ContactRecord{.body1 = body1.GetID().GetIndexAndSequenceNumber(),
                                         .body2 = body2.GetID().GetIndexAndSequenceNumber(),
                                         .added = true,
                                         .point = point,
                                         .normal = manifold.mWorldSpaceNormal});
    }
    void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override {
        apply_surface(body1, body2, manifold, settings);
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
    // Berührt ein Körper das Terrain, gelten Reibung und Rückprall des Materials unter ihm (wie
    // Jolt sie sonst aus den Körpern mischt: Wurzel des Produkts bzw. das Größere).
    void apply_surface(const JPH::Body& body1, const JPH::Body& body2,
                       const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) const {
        if (surfaces == nullptr) {
            return;
        }
        const auto mix = [&](const JPH::Body& terrain, JPH::SubShapeID id, const JPH::Body& other) {
            const auto material = terrain_material(*terrain.GetShape(), id);
            if (!material) {
                return false;
            }
            const SurfaceValues& surface = (*surfaces)[*material];
            settings.mCombinedFriction = std::sqrt(surface.friction * other.GetFriction());
            settings.mCombinedRestitution = std::max(surface.restitution, other.GetRestitution());
            return true;
        };
        if (!mix(body1, manifold.mSubShapeID1, body2)) {
            (void)mix(body2, manifold.mSubShapeID2, body1);
        }
    }

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
    // Eingaben des Jobs; er verbraucht sie.
    JPH::VertexList vertices;
    JPH::IndexedTriangleList input;
    JPH::PhysicsMaterialList materials;
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

// Eine Spielfigur (R4.3, PHYS-03).
struct CharacterRecord {
    JPH::Ref<JPH::CharacterVirtual> character;
    JPH::RefConst<JPH::Shape> standing;
    JPH::RefConst<JPH::Shape> crouching;
    CharacterController settings;
    // Zuletzt geschrieben; eine Abweichung ist eine Änderung von außen (Teleport).
    world::Transform transform;
    bool has_parent = false;
    bool crouched = false;
    bool supported = false;
    bool frozen = false;
    // Höchster Punkt seit dem Verlassen des Bodens (für CharacterLanded).
    f64 fall_top = 0.0;
    // Weg am Boden seit dem letzten Schritt.
    f64 step_distance = 0.0;
};

bool same_character(const CharacterController& a, const CharacterController& b) noexcept {
    return a.radius == b.radius && a.height == b.height && a.crouch_height == b.crouch_height &&
           a.max_slope == b.max_slope && a.step_height == b.step_height &&
           a.jump_height == b.jump_height && a.walk_speed == b.walk_speed &&
           a.run_speed == b.run_speed && a.crouch_speed == b.crouch_speed &&
           a.ground_acceleration == b.ground_acceleration &&
           a.air_acceleration == b.air_acceleration && a.gravity_factor == b.gravity_factor &&
           a.mass == b.mass && a.push_strength == b.push_strength && a.step_length == b.step_length;
}

// Aufrechte Kapsel mit dem Fußpunkt im Ursprung.
JPH::RefConst<JPH::Shape> character_shape(f32 radius, f32 height) {
    const f32 r = std::max(radius, 0.01f);
    const f32 h = std::max(height, 2.0f * r);
    const f32 half_cylinder = 0.5f * h - r;
    JPH::Ref<JPH::Shape> capsule;
    if (half_cylinder <= 0.001f) {
        capsule = created(JPH::SphereShapeSettings{r}.Create(), "Figur");
    } else {
        capsule = created(JPH::CapsuleShapeSettings{half_cylinder, r}.Create(), "Figur");
    }
    if (capsule == nullptr) {
        return nullptr;
    }
    JPH::RotatedTranslatedShapeSettings moved{JPH::Vec3{0.0f, 0.5f * h, 0.0f},
                                              JPH::Quat::sIdentity(), capsule};
    return created(moved.Create(), "Figur");
}

// Figuren laufen nicht gegen Sensoren. Auf einem Client (R5.3) bremsen Stellvertreter
// dynamischer Körper (loose), die sich gerade bewegen, die vorhergesagte Figur nicht: Sie stehen
// dort, wo sie vor rund 100 ms auf dem Server waren, und rollende Kugeln oder geschobene Kisten
// lösten sonst ständig Korrekturen aus. Ruhende Körper halten die Figur auf wie auf dem Server.
class CharacterBodyFilter final : public JPH::BodyFilter {
public:
    explicit CharacterBodyFilter(const std::unordered_set<JPH::uint32>* loose = nullptr)
        : loose_{loose} {}

    bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (body.IsSensor()) {
            return false;
        }
        if (loose_ == nullptr || loose_->empty() ||
            !loose_->contains(body.GetID().GetIndexAndSequenceNumber())) {
            return true;
        }
        return body.GetLinearVelocity().LengthSq() < moving_speed * moving_speed;
    }

    // Ab dieser Geschwindigkeit gilt ein Stellvertreter als bewegt (m/s).
    static constexpr float moving_speed = 0.3f;

private:
    const std::unordered_set<JPH::uint32>* loose_;
};

// Baut die Form eines Chunks aus seinem Netz (Positionen in Metern relativ zur Ecke) mit einem
// Material pro Dreieck (Index in materials). nullptr bei einem Netz ohne brauchbare Dreiecke.
JPH::Ref<JPH::Shape> terrain_shape(JPH::VertexList vertices, JPH::IndexedTriangleList triangles,
                                   JPH::PhysicsMaterialList materials) {
    if (triangles.empty()) {
        return nullptr;
    }
    JPH::MeshShapeSettings settings{std::move(vertices), std::move(triangles),
                                    std::move(materials)};
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        return nullptr;
    }
    return result.Get();
}

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
    // Ein Material pro Voxel-Material und ihre Werte aus der Tabelle (R4.3).
    std::array<JPH::Ref<TerrainSurface>, world::max_terrain_materials> surfaces;
    SurfaceTable surface_values{};

    world::ComponentId transform_id{};
    world::ComponentId body_id{};
    world::ComponentId collider_id{};
    world::ComponentId interpolated_id{};

    // Nach Kennung sortiert: Abgleich und Rückschrieb laufen immer in derselben Reihenfolge.
    std::map<u64, BodyRecord> bodies;
    // Kennung des Körpers (Index und Folgenummer) zu Entity, für Kontakte.
    std::unordered_map<JPH::uint32, u64> entity_of_body;
    // Stellvertreter dynamischer Körper auf einem Client (Modus remote, CharacterBodyFilter).
    std::unordered_set<JPH::uint32> loose_proxies;
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
    // Spielfiguren nach Kennung.
    std::map<u64, CharacterRecord> characters;
    world::ComponentId character_id{};
    world::ComponentId motion_id{};
    world::ComponentId predicted_id{};
    std::vector<CharacterLanded> landed_events;
    std::vector<CharacterStep> step_events;
    world::ComponentId terrain_state_id{};
    world::ComponentId world_transform_id{};
    // Einmal pro Entity warnen statt in jedem Schritt.
    std::unordered_map<u64, bool> warned;

    Impl(world::World& w, jobs::JobSystem& j, PhysicsSettings s);

    ~Impl();

    // Neue, leere Jolt-Welt. Jolt ist nur bitgleich, wenn auch der innere Zustand gleich ist
    // (Kennungen der Körper, Zwischenspeicher der Kontakte); nach dem Verwerfen aller Körper
    // beginnt deshalb eine frische Welt.
    void create_system();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    JPH::BodyInterface& body_interface() noexcept;

    void warn_once(u64 entity, std::string_view message);

    // Weltlage einer Entity (Position, Drehung, Skalierung).
    math::Decomposed world_pose(u64 entity, const world::Transform& transform, bool& has_parent);

    JPH::Ref<JPH::Shape> model_shape(u64 entity, const Collider& collider, ShapeType type,
                                     math::Vec3 scale);

    JPH::Ref<JPH::Shape> make_shape(u64 entity, const RigidBody& body, const Collider& collider,
                                    math::Vec3 scale);

    // Legt den Körper an; false, wenn die Form (noch) fehlt.
    bool create_body(u64 entity, const RigidBody& body, const Collider& collider,
                     const world::Transform& transform);

    void destroy_body(std::map<u64, BodyRecord>::iterator it, std::vector<ContactEnded>& ended,
                      std::vector<TriggerLeft>& left);

    [[nodiscard]] bool is_sensor_entity(u64 entity) const;

    void emit_end(u64 a, u64 b, std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left);

    void remove_all();

    void clear();

    void sync(std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left, f32 dt);

    // Änderungen von außen an einem bestehenden Körper.
    void update_body(u64 entity, BodyRecord& record, const RigidBody& body,
                     const world::Transform& transform, f32 dt);

    void write_back();

    void publish(std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left);

    // --- Spielfiguren (R4.3) ---------------------------------------------------------------------

    [[nodiscard]] JPH::BodyID body_id_of(u64 entity) const;

    void destroy_character(std::map<u64, CharacterRecord>::iterator it,
                           std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left);

    bool create_character(u64 entity, const CharacterController& controller,
                          const world::Transform& transform);

    // Legt Figuren an, entfernt und ändert sie (vor dem Schritt, nach den Körpern).
    void sync_characters(std::vector<ContactEnded>& ended, std::vector<TriggerLeft>& left);

    // Bewegt alle Figuren um dt (vor dem Schritt der Körper, damit Kisten den Schub im selben
    // Schritt spüren).
    void update_characters(f32 dt);
    // Ein Schritt einer Figur; events: Landen und Schritte melden (nicht beim Neurechnen, R5.3).
    void move_character(u64 entity, CharacterRecord& record, f32 dt, bool events);
    // Ob sich die Figur in dieser Physik bewegt (im Modus remote nur mit Predicted).
    [[nodiscard]] bool simulates_character(u64 entity) const;

    // --- Kollision des Terrains (R4.3) -----------------------------------------------------------

    void remove_chunk_body(TerrainChunk& chunk);

    // Lage aller Dinge, die Formen brauchen: dynamische Körper und Beobachter (Spielfiguren
    // folgen mit ihnen).
    std::vector<math::DVec3> terrain_anchors();

    // Chunks der Stufe 0 eines Terrains, die eine Form brauchen: mit Netz und im Radius um einen
    // Anker. Sortiert.
    std::vector<voxel::ChunkCoord> needed_chunks(const world::TerrainData& data, math::DVec3 base,
                                                 std::span<const math::DVec3> anchors) const;

    // Startet einen Job, der die Form aus dem Netz baut.
    void start_build(TerrainChunk& chunk, const world::ChunkMesh& mesh);

    // Setzt eine fertige Form ein; liefert, ob sich die Kollision geändert hat.
    bool finish_build(u64 terrain_entity, const TerrainRecord& terrain, voxel::ChunkCoord coord,
                      TerrainChunk& chunk, f64& build_ms, u32& built);

    // Hält die Formen aller Terrains aktuell.
    void sync_terrains();

    // Ob unter einer Lage die Kollision des Terrains fehlt: Der Chunk der Stufe 0 an der Lage
    // oder einer der beiden darunter hat ein Netz, aber noch keine Form dazu, oder ein endloses
    // Terrain hat dort noch gar keine Chunks der Stufe 0 geladen.
    [[nodiscard]] bool missing_ground(math::DVec3 position) const;

    // Friert dynamische Körper ohne Boden ein und taut sie auf, sobald er da ist.
    void update_frozen();

    JPH::Ref<JPH::Shape> query_shape(const QueryShape& shape) const;

    void set_terrain_materials(const world::TerrainMaterials& table);

    [[nodiscard]] u64 entity_of(JPH::BodyID id) const;

    RayHit ray_hit(const JPH::RRayCast& ray, const JPH::RayCastResult& result,
                   f32 max_distance) const;
};

} // namespace ngine::physics
