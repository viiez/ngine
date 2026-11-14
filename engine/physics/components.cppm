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

// Registriert die Komponenten der Physik in der Reflection (nach world::register_types()).
void register_types(reflect::TypeRegistry& types);

} // namespace ngine::physics
