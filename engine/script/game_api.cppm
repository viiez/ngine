module;

#include <vector>

// Dienste der Engine für Skripte (R4.4): Aktionen der Eingabe, Abfragen der Physik und die
// Ereignisse der Physik für die Skript-Komponenten. Interne Partition; die Tabelle GameApi steht
// in NativeApi und hat dieselbe Anordnung wie NGine.GameApi in managed/Game.cs.
module ngine.script:game_api;

import ngine.core;
import ngine.input;
import ngine.physics;
import ngine.world;
import ngine.audio;

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
    i32 shape = 0;
    f32 radius = 0.5f;
    f32 half_extents[3] = {0.5f, 0.5f, 0.5f};
    f32 half_height = 0.5f;
};

// Ereignis für Skripte; kind wie NGine.EventKind.
enum class EventKind : i32 {
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
    i32 (*action)(const u8* name, i32 length, NativeAction* out);
    i32 (*raycast)(const f64* origin, const f32* direction, f32 max_distance,
                   const NativeFilter* filter, NativeHit* out);
    i32 (*shape_cast)(const NativeShape* shape, const f64* origin, const f32* rotation,
                      const f32* direction, f32 max_distance, const NativeFilter* filter,
                      NativeHit* out);
    // Anzahl der Treffer; höchstens capacity Kennungen in out.
    i32 (*overlap)(const NativeShape* shape, const f64* position, const f32* rotation,
                   const NativeFilter* filter, u64* out, i32 capacity);
    // Kugel an point (Weltkoordinaten) aus dem Terrain abtragen (add = 0) oder auftragen, mit
    // material für Neues. 1: geändert, 0: keine Änderung, -1: kein Terrain.
    i32 (*terrain_edit)(u64 terrain, const f64* point, f32 radius, i32 add, i32 material);
    // Audio (R4.5). Klang oder Klangsatz einmal abspielen, an position (nullptr: ohne Ortung);
    // 1: angenommen, 0: kein Audio oder kein Klang.
    i32 (*sound_play)(u64 sound, const f64* position, i32 bus, f32 volume, f32 pitch);
    // AudioSource der Entity starten (play = 1) oder stoppen (0); 1: Audio vorhanden.
    i32 (*source_play)(u64 entity, i32 play);
    // 1, wenn die AudioSource der Entity gerade spielt.
    i32 (*source_playing)(u64 entity);
    void (*set_bus_volume)(i32 bus, f32 volume);
    f32 (*bus_volume)(i32 bus);
    // Klang der Schritte zu einem Material des Terrains (TerrainMaterial::footstep_sounds), 0 ohne.
    u64 (*footstep_sound)(u32 material);
};

[[nodiscard]] GameApi make_game_api();

void bind_script_input(const input::ActionState* actions);
// Im festen Schritt liefern Aktionen ActionState::fixed(): Flanken bleiben, bis ein Schritt sie
// gesehen hat.
void set_script_input_fixed(bool fixed);
void bind_script_physics(const physics::PhysicsWorld* physics);
// Audio und Materialtabelle für Skripte (R4.5); nullptr löst sie.
void bind_script_audio(audio::AudioSystem* audio, audio::AudioEngine* engine,
                       const world::TerrainMaterials* materials);
// Meldet die Ereignisse der Physik am Bus an; sie sammeln sich bis take_script_events().
void bind_script_events(events::EventBus* bus);
[[nodiscard]] std::vector<NativeEvent> take_script_events();

} // namespace ngine::script
