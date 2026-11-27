module;

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Skripte als Teil des Spiels (R4.4): Bauen mit dotnet build im Hintergrund, Neuladen bei
// geänderten Dateien mit Erhalt der Feldwerte (Hot Reload, SCR-04) und die Stufen der Skripte in
// der Spielschleife. Sandbox, Editor und Server nutzen dieselbe Klasse.
export module ngine.script:system;

import ngine.core;
import ngine.input;
import ngine.physics;
import ngine.audio;
import ngine.animation;
import ngine.reflect;
import ngine.world;
import :runtime;

export namespace ngine::script {

// Meldung des C#-Compilers mit Ort.
struct Diagnostic {
    std::string file;
    u32 line = 0;
    u32 column = 0;
    std::string code;
    std::string message;
    bool error = true;
};

struct BuildResult {
    bool ok = false;
    // Gebaute Assembly (nur bei ok).
    std::string assembly;
    std::vector<Diagnostic> diagnostics;
    // Ganze Ausgabe von dotnet, für Fehler ohne Ort.
    std::string output;
    // dotnet ließ sich nicht starten (kein .NET-SDK); output sagt warum.
    bool sdk_missing = false;
    f64 seconds = 0.0;
};

// Meldungen der Form „Datei(Zeile,Spalte): error CS1002: Text [Projekt]“, ohne Doppelte.
[[nodiscard]] std::vector<Diagnostic> parse_diagnostics(std::string_view output);

struct ScriptBuildDesc {
    // Projektdatei (.csproj). Die Assembly heißt wie die Datei.
    std::string project;
    // Ausgabe (Assembly) und Zwischendateien (artifacts/); dazu ngine.props für
    // Entwicklungsumgebungen.
    std::string output_dir;
    // Ordner mit NGine.Scripting.dll (RuntimeDesc::assembly_dir).
    std::string scripting_dir;
    // Erzeugte C#-Structs der Komponenten, die NGine.Scripting nicht kennt (z. B. die der
    // Anwendung); das Projekt bindet sie über $(NgineBindings) ein. Leer: keine.
    std::string bindings;
    std::string dotnet = "dotnet";
};

// Baut ein Skript-Projekt mit dotnet build in einem eigenen Thread.
class ScriptBuilder {
public:
    explicit ScriptBuilder(ScriptBuildDesc desc);
    ~ScriptBuilder();
    ScriptBuilder(const ScriptBuilder&) = delete;
    ScriptBuilder& operator=(const ScriptBuilder&) = delete;

    // Startet einen Bau. Läuft schon einer, folgt nach ihm ein neuer; nur dessen Ergebnis zählt.
    void start();
    [[nodiscard]] bool busy() const;
    // Ergebnis eines fertigen Baus, einmal.
    [[nodiscard]] std::optional<BuildResult> poll();
    // Baut sofort und wartet (Tests, Server ohne Hot Reload).
    [[nodiscard]] BuildResult build();
    // Wo die Assembly nach einem gelungenen Bau liegt.
    [[nodiscard]] std::string assembly_path() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct ScriptSystemDesc {
    // Projekt der Skripte, z. B. assets/skripte/Skripte.csproj. Leer: keine Skripte.
    std::string project;
    // Cache, z. B. assets/.cache/skripte. Dort liegt die zuletzt gebaute Assembly.
    std::string cache_dir;
    // Mit dem .NET-SDK bauen. Ohne (ausgeliefertes Spiel, Server) lädt das System nur die
    // Assembly aus dem Cache.
    bool build = true;
    // Geänderte .cs-Dateien neu bauen und laden (Hot Reload).
    bool watch = true;
    std::string dotnet = "dotnet";
    // Wo die Welt läuft (R5.5): ohne Netz alle Skripte, sonst nur die ihrer Rolle.
    ScriptRole role = ScriptRole::Local;
};

struct ScriptStatus {
    bool building = false;
    // Geladene Assembly; leer, wenn keine Skripte laufen.
    std::string assembly;
    // Wie oft Skripte geladen wurden (1 nach dem ersten Laden).
    u32 generation = 0;
    // Ergebnis des letzten Baus.
    std::optional<BuildResult> last_build;
    std::vector<std::string> classes;
};

// Skripte eines Spiels: bauen, laden, neu laden und in der Spielschleife ausführen.
class ScriptSystem {
public:
    ScriptSystem(ScriptRuntime& runtime, reflect::TypeRegistry& types, ScriptSystemDesc desc);
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // Lädt die Assembly aus dem Cache, falls es eine gibt, und startet den ersten Bau. Mit wait
    // wartet es auf den Bau (Sandbox ohne Fenster, Tests).
    void start(bool wait = false);
    // Einmal pro Bild im Hauptthread: geänderte Dateien, fertige Bauten, Neuladen.
    void update();
    // Lädt eine gebaute Assembly; vorher gehen die Felder der laufenden Skripte in ihre
    // Komponenten, danach entstehen die Skripte neu aus ihnen (Start läuft erneut).
    [[nodiscard]] bool reload(std::string_view assembly);
    // Baut neu (z. B. Knopf im Editor).
    void rebuild();

    // Welt und Dienste für die Skripte; nullptr löst sie. Vor dem Ende der Welt lösen.
    void bind(world::World* world, const input::ActionState* actions = nullptr,
              const physics::PhysicsWorld* physics = nullptr, events::EventBus* bus = nullptr);
    // Eigene Aktionen für die Skripte einer Entity (R5.1, Figuren der Spieler auf dem Server);
    // nullptr löst sie. bind(nullptr) löst alle.
    void bind_input(u64 entity, const input::ActionState* actions);
    // Audio und Materialtabelle für die Skripte (R4.5); nullptr löst sie.
    void bind_audio(audio::AudioSystem* audio, audio::AudioEngine* engine,
                    const world::TerrainMaterials* materials = nullptr);
    // Animation für die Skripte (R4.6); nullptr löst sie.
    void bind_animation(animation::AnimationSystem* animation);
    // Netz für die Skripte auf dem Server (R5.5, Net.Broadcast, Einmal-Klänge); muss gebunden
    // bleiben, bis nullptr es löst.
    void bind_net(const ScriptNet* net);
    // Ereignis vom Server an die Skripte einer Entity (R5.5, OnNetEvent), wenn aktiv.
    void deliver_net_event(u64 entity, std::string_view name, f32 value);
    // Felder der laufenden Skripte in ihre Komponenten bzw. aus ihnen (R5.5: replizierte
    // Skript-Komponenten vor und nach einem Schnappschuss).
    void store_fields();
    void load_fields();
    [[nodiscard]] ScriptRole role() const noexcept;
    // Andere Rolle, z. B. im Editor beim Spielen als Listen-Server (R5.5); nur, solange keine
    // Skripte laufen (nicht aktiv).
    void set_role(ScriptRole role);
    // Wählt die eigene Skript-Welt der Laufzeit für Aufrufe an runtime() (macht runtime() auch).
    void select();
    // FixedUpdate bzw. Update aller Skripte, wenn aktiv, gebunden und geladen.
    void run(ScriptPhase phase, f32 delta);
    // Hängt FixedUpdate als feste Stufe (vor der Physik, wenn vor physics::add_to() gerufen) und
    // Update als Stufe nach Phase::Frame an.
    void add_to(world::Simulation& simulation);
    // Nur aktive Skripte laufen (Editor: beim Spielen). Abschalten stoppt alle (Stop).
    void set_active(bool active);
    [[nodiscard]] bool active() const noexcept;
    // Außerhalb des Spiels: OnValidate der Skript-Komponenten (nach Inspector, Undo, Laden).
    // Läuft auch von selbst nach dem Neuladen und beim Abschalten. Im Spiel tut es nichts.
    void validate();

    [[nodiscard]] const ScriptStatus& status() const noexcept;
    // Laufzeit mit der Skript-Welt dieses Systems gewählt.
    [[nodiscard]] ScriptRuntime& runtime() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::script
