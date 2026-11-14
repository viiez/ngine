module;

#include <functional>
#include <memory>
#include <optional>
#include <vector>

export module ngine.physics:physics_world;

import ngine.core;
import ngine.math;
import ngine.jobs;
import ngine.world;
import :components;

export namespace ngine::physics {

struct PhysicsSettings {
    math::Vec3 gravity{0.0f, -9.81f, 0.0f};
    // Obergrenzen der Jolt-Welt; darüber werden Körper bzw. Kontakte nicht mehr angelegt.
    u32 max_bodies = 65536;
    u32 max_body_pairs = 65536;
    u32 max_contact_constraints = 16384;
    // Speicher für einen Schritt in Bytes.
    usize temp_memory = usize{16} << 20;
    // Teilschritte der Kollision pro festem Schritt (mehr hilft bei schnellen Körpern).
    u32 collision_steps = 1;
    // Kollision des Terrains (R4.3): Formen entstehen für Chunks der Stufe 0 in diesem Abstand
    // (Meter) um dynamische Körper, Spielfiguren und TerrainObserver.
    f32 terrain_radius = 24.0f;
    // So viele Schritte bleibt eine Form stehen, nachdem nichts sie mehr braucht.
    u32 terrain_keep_steps = 120;
    // Wartet im Abgleich, bis alle nötigen Formen gebaut sind. Dann ist ein Lauf bitgleich (Tests,
    // --headless, Server); im Spiel bleiben Körper ohne Form unter sich solange eingefroren.
    bool wait_for_terrain = false;
};

// Dreiecke eines Meshes für ConvexHull und Mesh. Indizes zu dritt, gegen den Uhrzeigersinn von
// außen gesehen.
struct TriangleMesh {
    std::vector<math::Vec3> positions;
    std::vector<u32> indices;
};

// Liefert die Dreiecke des Meshes mesh im Modell model, oder nichts, wenn es (noch) nicht da ist.
// Die Physik lädt keine Assets selbst; Sandbox und Editor geben eine Quelle aus dem Koch-Cache.
using ShapeSource = std::function<std::optional<TriangleMesh>(u64 model, u32 mesh)>;

// Ein Chunk mit Form der Kollision (terrain_shapes()).
struct TerrainChunkShape {
    u64 terrain = 0;
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    // Ecke des Chunks in der Welt und Kantenlänge in Metern.
    math::DVec3 corner{0.0, 0.0, 0.0};
    f32 size = 0.0f;
    u32 triangles = 0;
    // Eine neuere Fassung wird gerade gebaut.
    bool rebuilding = false;
};

// Welche Körper eine Abfrage sieht.
struct QueryFilter {
    u32 layers = all_layers;
    // Diese Entity zählt nicht (z. B. die Spielfigur, von der der Strahl ausgeht).
    u64 ignore = 0;
    // Sensoren zählen nur, wenn ihre Ebene in layers steht und dies gesetzt ist.
    bool sensors = false;
};

struct RayHit {
    // Bei Treffern auf dem Terrain die Entity des Terrains.
    u64 entity = 0;
    math::DVec3 point{0.0, 0.0, 0.0};
    math::Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 distance = 0.0f;
    // Voxel-Material des getroffenen Dreiecks auf dem Terrain (R4.3), sonst 0.
    u32 material = 0;
};

// Form für shape_cast() und overlap().
struct QueryShape {
    ShapeType shape = ShapeType::Sphere;
    f32 radius = 0.5f;
    math::Vec3 half_extents{0.5f, 0.5f, 0.5f};
    f32 half_height = 0.5f;
};

struct ShapeHit {
    u64 entity = 0;
    // Berührpunkt auf dem getroffenen Körper.
    math::DVec3 point{0.0, 0.0, 0.0};
    // Zeigt vom getroffenen Körper weg.
    math::Vec3 normal{0.0f, 1.0f, 0.0f};
    // Strecke bis zur Berührung.
    f32 distance = 0.0f;
};

// Ereignisse nach jedem Schritt, im Hauptthread über den EventBus, sortiert nach den Kennungen
// beider Entities (a < b). So reagiert das Gameplay in jeder Ausführung in derselben Reihenfolge.
struct ContactBegan {
    u64 a = 0;
    u64 b = 0;
    math::DVec3 point{0.0, 0.0, 0.0};
    // Von a nach b.
    math::Vec3 normal{0.0f, 1.0f, 0.0f};
};
struct ContactEnded {
    u64 a = 0;
    u64 b = 0;
};
struct TriggerEntered {
    u64 sensor = 0;
    u64 other = 0;
};
struct TriggerLeft {
    u64 sensor = 0;
    u64 other = 0;
};

// Zeiten des letzten Schritts in Millisekunden.
struct StepStats {
    f64 sync_ms = 0.0;
    f64 update_ms = 0.0;
    f64 write_back_ms = 0.0;
    u32 bodies = 0;
    u32 active_bodies = 0;
    u32 contacts = 0;
    // Kollision des Terrains (R4.3): Chunks mit Form, Formen im Bau, im letzten Schritt
    // eingesetzte Formen, mittlere Bauzeit einer Form, Speicher aller Formen.
    u32 terrain_chunks = 0;
    u32 terrain_building = 0;
    u32 terrain_built = 0;
    f64 terrain_build_ms = 0.0;
    usize terrain_bytes = 0;
    // Körper, die ohne Form des Terrains unter sich eingefroren sind.
    u32 frozen_bodies = 0;
    // Spielfiguren und die Zeit für ihre Bewegung im letzten Schritt.
    u32 characters = 0;
    f64 characters_ms = 0.0;
};

// Physik-Welt (PHYS-01, ADR 0045): Jolt mit Positionen in doppelter Genauigkeit, gerechnet auf dem
// Job-System der Engine. Die Welt ist die Quelle der Wahrheit für das, was gespeichert wird
// (RigidBody, Collider, Transform); die Physik-Welt hält nur den Zustand der Simulation und
// gleicht sich vor jedem Schritt ab:
//
// - Entities mit RigidBody und Collider bekommen einen Körper und dazu Interpolated, Entities ohne
//   verlieren ihn. Körper entstehen in der Reihenfolge der Kennungen, damit ein Lauf bei gleichen
//   Eingaben bitgleich ist, mit 1 wie mit 16 Threads.
// - Geänderte Werte (Inspector, Skripte, Befehle) gehen an Jolt; ein Transform, das jemand anders
//   als die Physik geändert hat, versetzt den Körper und weckt ihn.
// - Nach dem Schritt schreibt die Physik Lage und Geschwindigkeit aktiver dynamischer Körper in
//   Transform und RigidBody; kinematische Körper fahren zu ihrem Transform.
// - Terrains (world::Terrain) bekommen Formen aus den Netzen ihrer Chunks der Stufe 0, aber nur in
//   PhysicsSettings::terrain_radius um Körper, Spielfiguren und Beobachter (R4.3, VOX-08). Gebaut
//   wird in Jobs; ein dynamischer Körper, unter dem eine Form noch fehlt, bleibt eingefroren, statt
//   durchzufallen. Nach einem Pinsel ersetzt die neue Form die alte und weckt Körper in der Nähe.
//
// Läuft im Hauptthread nach den Systemen der festen Phase (add_to()). Kein Code außerhalb des
// Adapters kennt Jolt.
class PhysicsWorld {
public:
    PhysicsWorld(world::World& world, jobs::JobSystem& jobs, PhysicsSettings settings = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    void set_shape_source(ShapeSource source);
    // Ereignisse gehen an bus; ohne Bus werden sie nicht gesammelt.
    void set_events(events::EventBus* bus) noexcept;
    // Reibung und Rückprall der Materialien des Terrains (Asset, R4.3); gilt ab dem nächsten
    // Schritt, auch für schon gebaute Formen. Ohne Aufruf gilt world::standard_terrain_materials().
    void set_terrain_materials(const world::TerrainMaterials& table);
    // Wie PhysicsSettings::wait_for_terrain, z. B. für Bilder ohne Fenster.
    void set_wait_for_terrain(bool wait) noexcept;

    // Gleicht die Körper mit den Komponenten ab (step() tut das selbst).
    void sync();
    // Ein Schritt um dt Sekunden: Abgleich, Rechnen, Rückschrieb, Ereignisse.
    void step(f32 dt);
    // Verwirft alle Körper (z. B. beim Stoppen des Spielmodus); der nächste Abgleich legt sie neu
    // an.
    void clear();

    // Strahl von origin entlang direction (wird normiert) bis max_distance.
    [[nodiscard]] std::optional<RayHit> raycast(math::DVec3 origin, math::Vec3 direction,
                                                f32 max_distance, QueryFilter filter = {}) const;
    // Alle Treffer, nach Abstand sortiert.
    [[nodiscard]] std::vector<RayHit> raycast_all(math::DVec3 origin, math::Vec3 direction,
                                                  f32 max_distance, QueryFilter filter = {}) const;
    // Schiebt shape von origin entlang direction bis max_distance; liefert die erste Berührung.
    [[nodiscard]] std::optional<ShapeHit> shape_cast(const QueryShape& shape, math::DVec3 origin,
                                                     math::Quat rotation, math::Vec3 direction,
                                                     f32 max_distance,
                                                     QueryFilter filter = {}) const;
    // Entities, deren Formen shape an dieser Lage überlappen, nach Kennung sortiert.
    [[nodiscard]] std::vector<u64> overlap(const QueryShape& shape, math::DVec3 position,
                                           math::Quat rotation, QueryFilter filter = {}) const;

    // Ob die Entity einen Körper hat.
    [[nodiscard]] bool has_body(u64 entity) const noexcept;
    // Ob ihr Körper wach ist (statische und fehlende Körper: false).
    [[nodiscard]] bool is_active(u64 entity) const noexcept;
    // Ob ihr Körper eingefroren ist, weil unter ihm die Form des Terrains noch fehlt.
    [[nodiscard]] bool is_frozen(u64 entity) const noexcept;
    // Chunks eines Terrains mit fertiger Form (für Debug-Zeichnen und Tests), sortiert.
    [[nodiscard]] std::vector<TerrainChunkShape> terrain_shapes() const;
    [[nodiscard]] usize body_count() const noexcept;
    [[nodiscard]] const StepStats& stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Hängt den Schritt der Physik als Stufe an die feste Simulation, nach allen Systemen der Phase
// Fixed (Simulation::add_fixed_stage()). physics muss die Simulation überleben.
void add_to(world::Simulation& simulation, PhysicsWorld& physics);

// Prüfsumme über Lage und Geschwindigkeit aller Körper in der Reihenfolge der Kennungen (FNV-1a
// über die Bytes). Für Tests und --headless: gleiche Szene, gleiche Summe.
[[nodiscard]] u64 checksum(world::World& world);

} // namespace ngine::physics
