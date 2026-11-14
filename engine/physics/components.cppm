module;

export module ngine.physics:components;

import ngine.core;
import ngine.math;
import ngine.reflect;

export namespace ngine::physics {

// Wie sich ein Körper bewegt.
enum class MotionType : u8 {
    // Bewegt sich nie (Boden, Wände). Nimmt die Weltlage seiner Entity, auch mit Elternteil.
    Static,
    // Folgt seinem Transform, schiebt dynamische Körper, wird selbst nicht geschoben.
    Kinematic,
    // Fällt, stößt und rollt; die Physik schreibt seine Lage nach jedem Schritt in Transform.
    Dynamic,
};

// Kollisionsebene. Welche Ebenen sich treffen, steht fest im Code (layers_collide()).
enum class PhysicsLayer : u8 {
    // Unbewegliches. Statische Körper landen immer hier, außer Sensoren.
    Static,
    // Bewegte Körper: trifft alles.
    Moving,
    // Spielfiguren (R4.3): wie Moving.
    Character,
    // Trümmer und Kleinkram: trifft nur Static, damit viele davon billig bleiben.
    Debris,
    // Auslöser: meldet Überlappungen mit Moving und Character, stößt nichts.
    Sensor,
};

inline constexpr u32 layer_count = 5;

[[nodiscard]] constexpr u32 layer_bit(PhysicsLayer layer) noexcept {
    return u32{1} << static_cast<u32>(layer);
}
inline constexpr u32 all_layers = (u32{1} << layer_count) - 1;

// Ob Körper der beiden Ebenen zusammenstoßen bzw. sich melden (Sensor).
[[nodiscard]] constexpr bool layers_collide(PhysicsLayer a, PhysicsLayer b) noexcept {
    const auto pair = [&](PhysicsLayer x, PhysicsLayer y) {
        return (a == x && b == y) || (a == y && b == x);
    };
    if (a == PhysicsLayer::Static && b == PhysicsLayer::Static) {
        return false;
    }
    if (a == PhysicsLayer::Sensor || b == PhysicsLayer::Sensor) {
        return pair(PhysicsLayer::Sensor, PhysicsLayer::Moving) ||
               pair(PhysicsLayer::Sensor, PhysicsLayer::Character);
    }
    if (a == PhysicsLayer::Debris || b == PhysicsLayer::Debris) {
        return pair(PhysicsLayer::Debris, PhysicsLayer::Static);
    }
    return true;
}

// Starrer Körper (PHYS-02). Zusammen mit Collider wird die Entity ein Körper der Physik-Welt. Die
// Geschwindigkeiten stehen in der Komponente: Die Physik schreibt sie nach jedem Schritt zurück,
// eine Szene kann so einen Wurf speichern, und die Sicherung des Spielmodus enthält alles.
struct RigidBody {
    MotionType motion = MotionType::Dynamic;
    PhysicsLayer layer = PhysicsLayer::Moving;
    // Masse in kg; 0 heißt: aus dem Volumen der Form mit der Dichte von Wasser (1000 kg/m³).
    f32 mass = 0.0f;
    f32 friction = 0.5f;
    // Rückprall: 0 bleibt liegen, 1 springt ohne Verlust.
    f32 restitution = 0.0f;
    f32 linear_damping = 0.05f;
    f32 angular_damping = 0.05f;
    f32 gravity_factor = 1.0f;
    // Darf einschlafen, wenn er zur Ruhe kommt (spart Rechenzeit).
    bool allow_sleep = true;
    // m/s in Weltrichtungen.
    math::Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    // rad/s um Weltachsen.
    math::Vec3 angular_velocity{0.0f, 0.0f, 0.0f};
};

enum class ShapeType : u8 {
    Sphere,
    Box,
    Capsule,
    // Konvexe Hülle der Eckpunkte eines Meshes (model, mesh).
    ConvexHull,
    // Dreiecksnetz eines Meshes (model, mesh); nur für statische und kinematische Körper.
    Mesh,
};

// Kollisionsform. Maße gelten vor der Skalierung aus Transform; eine andere Skalierung baut die
// Form neu. Kapseln stehen entlang der lokalen y-Achse.
struct Collider {
    ShapeType shape = ShapeType::Box;
    // Kugel und Kapsel.
    f32 radius = 0.5f;
    // Quader.
    math::Vec3 half_extents{0.5f, 0.5f, 0.5f};
    // Kapsel: halbe Länge des Zylinders zwischen den Halbkugeln.
    f32 half_height = 0.5f;
    // Versatz der Form gegenüber der Entity, in lokalen Koordinaten.
    math::Vec3 offset{0.0f, 0.0f, 0.0f};
    // Konvexe Hülle und Netz: Kennung des Modells und Index des Meshes darin.
    u64 model = 0;
    u32 mesh = 0;
    // Auslöser: meldet Überlappungen (TriggerEntered, TriggerLeft), stößt nichts.
    bool sensor = false;
};

// Spielfigur (PHYS-03, R4.3): eine aufrechte Kapsel, die läuft, Hänge und Stufen nimmt, springt
// und Kisten schiebt. Die Lage der Entity (Transform::position) ist der Fußpunkt; die Physik dreht
// die Figur nicht, die Steuerung setzt die Drehung um die Hochachse selbst. Gesteuert wird über
// CharacterMotion. Zusammen mit RigidBody oder Collider ergibt sie keinen zweiten Körper; dann
// gilt nur die Figur.
struct CharacterController {
    // Kapsel: Radius und ganze Höhe in Metern (stehend und geduckt).
    f32 radius = 0.35f;
    f32 height = 1.8f;
    f32 crouch_height = 1.1f;
    // Steilster begehbarer Hang (Bogenmaß).
    f32 max_slope = 0.8727f;
    // Höchste Stufe, die die Figur ohne Springen nimmt.
    f32 step_height = 0.4f;
    // Sprunghöhe in Metern.
    f32 jump_height = 1.2f;
    // Geschwindigkeiten in m/s.
    f32 walk_speed = 4.0f;
    f32 run_speed = 7.0f;
    f32 crouch_speed = 2.0f;
    // Wie schnell die Figur ihre Wunschgeschwindigkeit erreicht (m/s²), am Boden und in der Luft.
    f32 ground_acceleration = 40.0f;
    f32 air_acceleration = 6.0f;
    f32 gravity_factor = 1.0f;
    // Masse in kg (drückt auf Kisten, auf denen sie steht) und größte Schubkraft in N.
    f32 mass = 70.0f;
    f32 push_strength = 200.0f;
    // Weg am Boden zwischen zwei Schritten (CharacterStep) in Metern.
    f32 step_length = 0.7f;
};

// Ob und worauf eine Spielfigur steht.
enum class GroundState : u8 {
    // In der Luft.
    Air,
    // Auf begehbarem Boden.
    Ground,
    // Auf zu steilem Boden: rutscht.
    Steep,
};

// Laufzeitzustand einer Spielfigur (transient): Die Steuerung schreibt die Wünsche, die Physik
// den Rest. Szenen speichern ihn nicht; die Physik legt ihn an.
struct CharacterMotion {
    // Gewünschte Bewegung in Weltrichtungen auf der Ebene (Länge 0 bis 1, y zählt nicht).
    math::Vec3 move{0.0f, 0.0f, 0.0f};
    bool run = false;
    bool crouch = false;
    // Springen, sobald die Figur steht; die Physik setzt es danach zurück.
    bool jump = false;
    // Von der Physik: Geschwindigkeit, Boden, ob sie geduckt ist.
    math::Vec3 velocity{0.0f, 0.0f, 0.0f};
    GroundState ground = GroundState::Air;
    math::Vec3 ground_normal{0.0f, 1.0f, 0.0f};
    // Entity unter der Figur (Terrain oder Körper), 0 in der Luft.
    u64 ground_entity = 0;
    // Material des Bodens (Nummer der Materialtabelle des Terrains, 0 bei Körpern).
    u32 ground_material = 0;
    bool crouched = false;
};

// Ereignisse der Spielfiguren nach jedem Schritt über den EventBus, nach Kennung sortiert.
struct CharacterLanded {
    u64 entity = 0;
    // Höhe des Falls in Metern und Geschwindigkeit beim Aufsetzen in m/s.
    f32 height = 0.0f;
    f32 speed = 0.0f;
    u32 material = 0;
};
struct CharacterStep {
    u64 entity = 0;
    math::DVec3 position{0.0, 0.0, 0.0};
    u32 material = 0;
    bool run = false;
};

// Registriert die Komponenten der Physik in der Reflection (nach world::register_types()).
void register_types(reflect::TypeRegistry& types);

} // namespace ngine::physics
