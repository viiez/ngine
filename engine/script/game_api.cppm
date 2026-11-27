module;

#include <string>
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
import ngine.animation;
import :runtime;

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
    // Ereignis eines Zustands der Animation (R4.6); a: Entity mit dem Skript, b: Entity mit dem
    // Animator, text: Name des Ereignisses.
    Animation,
    // Ereignis vom Server (R5.5, Net.Broadcast); a: Entity, text: Name, value: Wert.
    Net,
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
    // Animation: Name des Ereignisses, gültig während der Zustellung.
    const u8* text = nullptr;
    i32 text_length = 0;
    i32 reserved = 0;
};

struct GameApi {
    // 1, wenn es die Aktion gibt; Wert in *out. entity ist die Entity des laufenden Skripts: Sind
    // für sie eigene Aktionen gebunden (R5.1), gelten diese, sonst die allgemeinen.
    i32 (*action)(u64 entity, const u8* name, i32 length, NativeAction* out);
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
    // Animation (R4.6). Parameter des Animators an entity oder dem ersten Nachkommen mit Animator
    // setzen; kind 0: Zahl, 1: Wahrheitswert, 2: Auslöser. 1: angenommen, 0: kein Animator oder
    // unbekannter Parameter.
    i32 (*anim_set)(u64 entity, const u8* name, i32 length, i32 kind, f32 value);
    // Aktiver Zustand als UTF-8 in out (höchstens capacity Bytes); liefert die volle Länge, -1 ohne
    // Animator.
    i32 (*anim_state)(u64 entity, u8* out, i32 capacity);
    // Netz (R5.5). Ereignis an die Clients: player 0 an alle, die entity kennen, sonst an den
    // Spieler, dem die Figur player gehört. 1: angenommen, 0: kein Netz (ohne Server).
    i32 (*net_send)(u64 entity, u64 player, const u8* name, i32 length, f32 value);
};

[[nodiscard]] GameApi make_game_api();

void bind_script_input(const input::ActionState* actions);
// Eigene Aktionen für die Skripte einer Entity (R5.1); nullptr löst sie.
void bind_script_input(u64 entity, const input::ActionState* actions);
void clear_script_entity_input();
// Im festen Schritt liefern Aktionen ActionState::fixed(): Flanken bleiben, bis ein Schritt sie
// gesehen hat.
void set_script_input_fixed(bool fixed);
void bind_script_physics(const physics::PhysicsWorld* physics);
// Audio und Materialtabelle für Skripte (R4.5); nullptr löst sie.
void bind_script_audio(audio::AudioSystem* audio, audio::AudioEngine* engine,
                       const world::TerrainMaterials* materials);
// Animation für Skripte (R4.6); nullptr löst sie.
void bind_script_animation(animation::AnimationSystem* animation);
// Netz für Skripte auf dem Server (R5.5); nullptr löst es.
void bind_script_net(const ScriptNet* net);
// Meldet die Ereignisse der Physik und der Animation am Bus an; sie sammeln sich bis
// take_script_events() bzw. take_script_animation_events().
void bind_script_events(events::EventBus* bus);
[[nodiscard]] std::vector<NativeEvent> take_script_events();
// Ereignisse der Animation seit dem letzten Aufruf, je Vorfahre des Animators eins (für Update).
// Die Namen bleiben bis zum nächsten Aufruf gültig.
[[nodiscard]] std::vector<NativeEvent> take_script_animation_events();

// Vergisst den Zustand einer Skript-Welt in dieser Partition (R5.5).
void drop_game_domain(u32 domain);

} // namespace ngine::script
