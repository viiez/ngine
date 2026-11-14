module;

#include <cstdint>
#include <vector>

// Dienste der Engine für Skripte (R4.4): Aktionen der Eingabe, Abfragen der Physik und die
// Ereignisse der Physik für die Skript-Komponenten. Interne Partition; die Tabelle GameApi steht
// in NativeApi und hat dieselbe Anordnung wie NGine.GameApi in managed/Game.cs.
module ngine.script:game_api;

import ngine.core;
import ngine.input;
import ngine.physics;

namespace ngine::script {

// Wie input::ActionValue, mit fester Anordnung für C#.
struct NativeAction {
    f32 x = 0.0f;
    f32 y = 0.0f;
    u8 down = 0;
    u8 pressed = 0;
    u8 released = 0;
    u8 reserved = 0;
};

struct NativeFilter {
    u32 layers = physics::all_layers;
    u32 sensors = 0;
    u64 ignore = 0;
};

struct NativeHit {
    u64 entity = 0;
    f64 point[3] = {};
    f32 normal[3] = {};
    f32 distance = 0.0f;
    u32 material = 0;
    u32 reserved = 0;
};

struct NativeShape {
    std::int32_t shape = 0;
    f32 radius = 0.5f;
    f32 half_extents[3] = {0.5f, 0.5f, 0.5f};
    f32 half_height = 0.5f;
};

// Ereignis für Skripte; kind wie NGine.EventKind.
enum class EventKind : std::int32_t {
    ContactBegan,
    ContactEnded,
    TriggerEntered,
    TriggerLeft,
    Landed,
    Step,
};

struct NativeEvent {
    EventKind kind = EventKind::ContactBegan;
    u32 material = 0;
    u64 a = 0;
    u64 b = 0;
    f64 point[3] = {};
    f32 normal[3] = {};
    // Landen: Fallhöhe; Schritt: 1, wenn die Figur läuft.
    f32 value = 0.0f;
};

struct GameApi {
    // 1, wenn es die Aktion gibt; Wert in *out.
    std::int32_t (*action)(const u8* name, std::int32_t length, NativeAction* out);
    std::int32_t (*raycast)(const f64* origin, const f32* direction, f32 max_distance,
                            const NativeFilter* filter, NativeHit* out);
    std::int32_t (*shape_cast)(const NativeShape* shape, const f64* origin, const f32* rotation,
                               const f32* direction, f32 max_distance, const NativeFilter* filter,
                               NativeHit* out);
    // Anzahl der Treffer; höchstens capacity Kennungen in out.
    std::int32_t (*overlap)(const NativeShape* shape, const f64* position, const f32* rotation,
                            const NativeFilter* filter, u64* out, std::int32_t capacity);
};

[[nodiscard]] GameApi make_game_api();

void bind_script_input(const input::ActionState* actions);
void bind_script_physics(const physics::PhysicsWorld* physics);
// Meldet die Ereignisse der Physik am Bus an; sie sammeln sich bis take_script_events().
void bind_script_events(events::EventBus* bus);
[[nodiscard]] std::vector<NativeEvent> take_script_events();

} // namespace ngine::script
