module;

#include <expected>
#include <functional>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// C#-Scripting über eingebettetes .NET (ADR 0006, ADR 0023). Durchstich D2: Laufzeit über hostfxr
// starten, Skripte aus einer Assembly in einen entladbaren Kontext laden, Aufrufe in beide
// Richtungen. Läuft ohne Fenster und GPU.
export module ngine.script:runtime;

import ngine.core;
import ngine.math;
import ngine.input;
import ngine.physics;
import ngine.audio;
import ngine.animation;
import ngine.reflect;
import ngine.world;

export namespace ngine::script {

template <typename T>
using Result = std::expected<T, std::string>;

// Gleiches Layout wie NGine.Position in C#. Daten, die beide Seiten teilen, sind blittable.
struct Position {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
};

// Wann Skripte laufen: im festen Schritt vor der Physik oder einmal pro Bild.
enum class ScriptPhase : u8 { Fixed, Frame };

// Wo eine Skript-Welt läuft (R5.5, NGine.NetRole): ohne Netz alle Skripte, als Server die ohne
// [Client], als Client die mit [Client] oder [Everywhere].
enum class ScriptRole : u8 { Local = 0, Server = 1, Client = 2 };

// Was Skripte auf dem Server ins Netz schicken (R5.5). Die Anwendung (app::GameServer) setzt die
// Funktionen; leere Funktionen verwerfen.
struct ScriptNet {
    // Net.Broadcast (player 0: an alle Clients, die entity kennen) bzw. Net.Send (an den Spieler,
    // dem die Figur player gehört).
    std::function<void(u64 entity, u64 player, std::string_view name, f32 value)> event;
    // Audio.Play ohne Audio-Gerät: Einmal-Klang für die Clients in Hörweite (ohne Ort an alle).
    std::function<void(u64 sound, std::optional<math::DVec3> position, audio::AudioBus bus,
                       f32 volume, f32 pitch)>
        sound;
};

struct RuntimeDesc {
    // Ordner mit NGine.Scripting.dll und NGine.Scripting.runtimeconfig.json.
    std::string assembly_dir;
    // Installationsordner von .NET. Leer: DOTNET_ROOT, dann die üblichen Orte des Systems.
    std::string dotnet_root;
};

// Die .NET-Laufzeit im Prozess. Es gibt höchstens eine; .NET lässt sich nicht entladen und im
// selben Prozess nicht mit anderen Einstellungen neu starten. Gehört einem Thread.
class ScriptRuntime {
public:
    [[nodiscard]] static Result<std::unique_ptr<ScriptRuntime>> create(const RuntimeDesc& desc);

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;
    ~ScriptRuntime();

    // Lädt alle Skripte (Klassen mit NGine.IScript) aus einer Assembly. Liefert ihre Anzahl.
    [[nodiscard]] Result<u32> load_scripts(std::string_view assembly_path);
    // Stoppt die Skripte und entlädt ihre Assembly. true, wenn .NET den Kontext freigegeben hat.
    [[nodiscard]] Result<bool> unload_scripts();

    [[nodiscard]] bool scripts_loaded() const noexcept;
    // Ordner mit NGine.Scripting.dll (RuntimeDesc::assembly_dir).
    [[nodiscard]] const std::string& assembly_dir() const noexcept;

    [[nodiscard]] Result<void> start();
    // Skripte sehen und ändern positions direkt, ohne Kopie.
    [[nodiscard]] Result<void> update(f32 delta_time, std::span<Position> positions);

    // --- Skript-Komponenten (R4.4) ------------------------------------------------------------

    // Registry, in der jede Skript-Klasse beim Laden ein Komponententyp wird (dieselbe, die die
    // Welt nutzt). Ohne Registry laden nur die Skripte aus D2 (IScript).
    void use_types(reflect::TypeRegistry* types);
    // Typen der geladenen Skript-Klassen, nach Namen sortiert.
    [[nodiscard]] std::span<const reflect::TypeInfo* const> script_types() const noexcept;
    // Ruft FixedUpdate bzw. Update aller Skript-Komponenten der gebundenen Welt auf (neue bekommen
    // vorher Start, verschwundene Stop) und führt danach ihre Strukturänderungen aus.
    [[nodiscard]] Result<void> run(ScriptPhase phase, f32 delta_time);
    // Aktionen für NGine.Input und Physik für NGine.Physics; nullptr löst sie. Mit bus erreichen
    // Kontakte, Auslöser, Landungen und Schritte die Skripte (vor dem nächsten FixedUpdate).
    void bind_input(const input::ActionState* actions);
    // Eigene Aktionen für die Skripte einer Entity, z. B. die Figur eines Spielers auf dem Server
    // (R5.1); sie gehen den allgemeinen vor. nullptr löst sie, entity 0 löst alle.
    void bind_input(u64 entity, const input::ActionState* actions);
    void bind_physics(const physics::PhysicsWorld* physics, events::EventBus* bus = nullptr);
    // Audio für NGine.Audio und die Materialtabelle für Schritte (R4.5); nullptr löst sie.
    void bind_audio(audio::AudioSystem* audio, audio::AudioEngine* engine,
                    const world::TerrainMaterials* materials = nullptr);
    // Animation für NGine.Animator (R4.6); nullptr löst sie.
    void bind_animation(animation::AnimationSystem* animation);
    // Felder der laufenden Skripte in ihre Komponenten schreiben (vor Speichern, Snapshot,
    // Inspector) bzw. aus ihnen lesen (nach Änderungen im Inspector, Undo).
    void store_fields();
    void load_fields();
    // Stoppt alle laufenden Skripte (Stop) und vergisst sie, z. B. beim Verlassen des
    // Spielmodus. Beim nächsten run() entstehen sie neu aus den Komponenten.
    void reset_instances();
    // Ruft OnValidate der Skript-Komponenten auf frischen Instanzen (außerhalb des Spiels, z. B.
    // nach Änderungen im Inspector), damit andere Komponenten zu den Feldern passen.
    void validate();

    // Welt, auf die Skripte über Entity und World zugreifen (R4.4); nullptr löst sie. Die Welt
    // muss leben, solange sie gebunden ist, und vor ihrem Ende gelöst werden.
    void bind_world(world::World* world);
    // Führt die Strukturänderungen aus, die Skripte gesammelt haben (Create, Set einer neuen
    // Komponente, Remove, Destroy, Parent, Name). Nicht während Systeme laufen.
    void apply_commands();
    // Vergleicht Größe und Offsets der erzeugten C#-Structs mit der Reflection. Typen, die nur
    // eine Seite kennt, fallen mit require_all als Fehler auf (veraltete Bindings).
    [[nodiscard]] Result<void> verify_bindings(const reflect::TypeRegistry& types,
                                               bool require_all = false);
    // Namen der Structs und Enums, die NGine.Scripting mitbringt.
    [[nodiscard]] std::vector<std::string> bound_types();
    // Ruft eine statische Methode ohne Parameter aus den geladenen Skripten auf
    // ("Namespace.Typ.Methode").
    [[nodiscard]] Result<void> invoke(std::string_view method);

    // --- Skript-Welten (R5.5) -------------------------------------------------------------------

    // Neue Skript-Welt mit eigenen Bindungen, Instanzen und Typ-Indizes; sie ist danach gewählt.
    // Hat die Laufzeit schon Skripte geladen, gibt es deren Klassen erst nach adopt_scripts().
    [[nodiscard]] u32 create_domain(ScriptRole role);
    // Wählt die Welt, auf die alle weiteren Aufrufe wirken. 0 ist die Welt ohne ScriptSystem.
    void select_domain(u32 domain);
    // Stoppt die Instanzen der Welt und vergisst sie; danach ist Welt 0 gewählt.
    void destroy_domain(u32 domain);
    [[nodiscard]] u32 domain() const noexcept;
    [[nodiscard]] ScriptRole role() const noexcept;
    [[nodiscard]] usize domain_count() const noexcept;
    // Meldet die Klassen der schon geladenen Assembly in der Registry der gewählten Welt an.
    [[nodiscard]] Result<void> adopt_scripts();
    // Was die Skripte der gewählten Welt ins Netz schicken; nullptr löst es.
    void bind_net(const ScriptNet* net);
    // Stellt ein Ereignis vom Server den Skripten einer Entity der gewählten Welt zu
    // (OnNetEvent) und führt danach ihre Strukturänderungen aus.
    [[nodiscard]] Result<void> deliver_net_event(u64 entity, std::string_view name, f32 value);

    // Prüft den Weg C++ → C# → C++.
    [[nodiscard]] i32 add(i32 a, i32 b);
    // Mittlere Kosten eines Aufrufs in Nanosekunden.
    [[nodiscard]] f64 measure_calls_to_managed(u32 count);
    [[nodiscard]] f64 measure_calls_to_native(u32 count);

    // Nur für create(); Impl ist außerhalb des Moduls nicht bekannt.
    struct Impl;
    explicit ScriptRuntime(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::script
