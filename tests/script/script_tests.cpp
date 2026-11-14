#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import ngine.core;
import ngine.filesystem;
import ngine.input;
import ngine.jobs;
import ngine.math;
import ngine.reflect;
import ngine.world;
import ngine.physics;
import ngine.platform;
import ngine.render;
import ngine.script;

// Diese Tests brauchen das .NET-SDK beim Bauen und .NET 10 zur Laufzeit. Fehlt beides, werden
// sie übersprungen; ist NGINE_REQUIRE_DOTNET gesetzt (CI), schlagen sie stattdessen fehl.

using namespace ngine;

namespace {

bool dotnet_required() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* value = std::getenv("NGINE_REQUIRE_DOTNET");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value != nullptr && *value != '\0';
}

// Die Assemblies liegen im Ordner „dotnet“ neben dem Testprogramm (TD-12); ohne .NET-SDK gibt es
// keine.
const std::string& dotnet_directory() {
    static const std::string directory = [] {
#ifdef NGINE_WITH_DOTNET
        if (auto exe = filesystem::executable_dir()) {
            return filesystem::join(*exe, "dotnet");
        }
#endif
        return std::string{};
    }();
    return directory;
}

// Sammelt Logzeilen, um zu prüfen, dass C# über die Engine schreibt.
class CaptureSink final : public log::Sink {
public:
    void write(const log::Record& record) override {
        const std::scoped_lock lock{mutex_};
        lines_.emplace_back(record.message);
    }
    [[nodiscard]] bool contains(const std::string& text) {
        const std::scoped_lock lock{mutex_};
        for (const auto& line : lines_) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

std::shared_ptr<CaptureSink> capture() {
    static const auto sink = [] {
        auto s = std::make_shared<CaptureSink>();
        log::add_sink(s);
        return s;
    }();
    return sink;
}

// .NET lässt sich im Prozess nur einmal starten; alle Tests teilen sich eine Laufzeit.
script::ScriptRuntime& runtime() {
    static std::unique_ptr<script::ScriptRuntime> instance;
    // Andere Tests (log_tests) ersetzen die Sinks; vor jedem Test wieder anhängen.
    log::remove_sink(capture());
    log::add_sink(capture());
    if (!instance) {
        if (dotnet_directory().empty()) {
            if (dotnet_required()) {
                FAIL("Ohne .NET-SDK gebaut (NGINE_WITH_DOTNET=OFF)");
            }
            SKIP("Ohne .NET-SDK gebaut");
        }
        auto created = script::ScriptRuntime::create({.assembly_dir = dotnet_directory()});
        if (!created) {
            if (dotnet_required()) {
                FAIL(".NET-Laufzeit startet nicht: " + created.error());
            }
            SKIP(".NET-Laufzeit startet nicht: " + created.error());
        }
        instance = std::move(*created);
    }
    return *instance;
}

std::string test_scripts() {
    return dotnet_directory() + "/Tests.Scripts.dll";
}

} // namespace

TEST_CASE("C++ ruft C# auf und bekommt das Ergebnis", "[script]") {
    auto& rt = runtime();
    CHECK(rt.add(2, 3) == 5);
    CHECK(rt.add(-7, 7) == 0);
    CHECK(capture()->contains("C#-Laufzeit bereit"));
}

TEST_CASE("Es gibt nur eine Laufzeit pro Prozess", "[script]") {
    (void)runtime();
    const auto second = script::ScriptRuntime::create({.assembly_dir = dotnet_directory()});
    REQUIRE_FALSE(second);
    CHECK(second.error().find("schon") != std::string::npos);
}

TEST_CASE("Skripte laden, loggen, Daten aendern und wieder entladen", "[script]") {
    auto& rt = runtime();
    const auto count = rt.load_scripts(test_scripts());
    REQUIRE(count);
    // Greeter und Mover (D2), Bumper, Spawner und Spinner (Skript-Komponenten), nicht die
    // abstrakte Klasse.
    CHECK(*count == 5);

    REQUIRE(rt.start());
    CHECK(capture()->contains("Hallo aus C#"));

    std::vector<script::Position> positions(3);
    REQUIRE(rt.update(0.5f, positions));
    REQUIRE(rt.update(0.5f, positions));
    CHECK(positions[0].y == 1.0f);
    CHECK(positions[2].x == 4.0f);
    CHECK(positions[2].z == 0.0f);

    const auto unloaded = rt.unload_scripts();
    REQUIRE(unloaded);
    CHECK(*unloaded); // Der Kontext ist wirklich frei (Voraussetzung für Hot Reload)
    CHECK(capture()->contains("Tschüss aus C#"));

    // Neu laden wie beim Hot Reload.
    const auto again = rt.load_scripts(test_scripts());
    REQUIRE(again);
    CHECK(*again == 5);
    REQUIRE(rt.unload_scripts());
}

TEST_CASE("Fehlende Skript-Assembly meldet einen Fehler", "[script]") {
    auto& rt = runtime();
    const auto result = rt.load_scripts(dotnet_directory() + "/Gibt.Es.Nicht.dll");
    CHECK_FALSE(result);
    // Danach lassen sich weiter Skripte laden.
    REQUIRE(rt.load_scripts(test_scripts()));
    REQUIRE(rt.unload_scripts());
}

TEST_CASE("Aufrufe zwischen C++ und C# sind billig", "[script]") {
    auto& rt = runtime();
    const f64 to_managed = rt.measure_calls_to_managed(100'000);
    const f64 to_native = rt.measure_calls_to_native(100'000);
    INFO("C++ -> C#: " << to_managed << " ns, C# -> C++: " << to_native << " ns");
    CHECK(to_managed > 0.0);
    CHECK(to_native > 0.0);
    // Großzügige Grenze, auch für Debug-Builds unter Last: ein Aufruf unter einer Mikrosekunde.
    CHECK(to_managed < 1000.0);
    CHECK(to_native < 1000.0);
}

namespace {

struct BindingSample {
    f32 speed = 2.5f;
    bool active = true;
    std::string label;
    math::Vec3 offset{0.0f, 1.0f, 0.0f};
    u8 mode = 3;
};

enum class BindingMode : u8 { Off, On };

void register_engine_types(reflect::TypeRegistry& types) {
    world::register_types(types);
    physics::register_types(types);
    render::register_types(types);
}

} // namespace

TEST_CASE("Bindings mirror the reflection layout with C++ defaults", "[script]") {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<BindingMode>("BindingMode")
        .value("Off", BindingMode::Off)
        .value("On", BindingMode::On);
    types.add<BindingSample>("BindingSample")
        .field("speed", &BindingSample::speed)
        .field("active", &BindingSample::active)
        .field("label", &BindingSample::label)
        .field("offset", &BindingSample::offset)
        .field("mode", &BindingSample::mode);
    const std::string source = script::generate_bindings(types);
    INFO(source);
    CHECK(source.find("public enum BindingMode : byte") != std::string::npos);
    CHECK(source.find("On = 1,") != std::string::npos);
    CHECK(source.find(std::format("[StructLayout(LayoutKind.Explicit, Size = {})]\npublic struct "
                                  "BindingSample : IComponent",
                                  sizeof(BindingSample))) != std::string::npos);
    CHECK(source.find(std::format("[FieldOffset({})] public float Speed;",
                                  offsetof(BindingSample, speed))) != std::string::npos);
    CHECK(source.find(std::format("[FieldOffset({})] public Vec3 Offset;",
                                  offsetof(BindingSample, offset))) != std::string::npos);
    CHECK(source.find("public byte Mode;") != std::string::npos);
    // Texte fehlen im Struct; der Typ ist nicht trivial.
    CHECK(source.find("public string") == std::string::npos);
    CHECK(source.find("Label") == std::string::npos);
    // Standardwerte aus C++.
    CHECK(source.find("Speed = 2.5f;") != std::string::npos);
    CHECK(source.find("Active = true;") != std::string::npos);
    CHECK(source.find("Offset.Y = 1f;") != std::string::npos);
    CHECK(source.find("Mode = (byte)3UL;") != std::string::npos);
    // Mathe-Typen bringt NGine.Scripting mit; ihre Anordnung wird trotzdem gemeldet.
    CHECK(source.find("public struct Vec3") == std::string::npos);
    CHECK(source.find("w.Type(\"Vec3\"") != std::string::npos);
    CHECK(source.find("public struct Transform : IComponent") != std::string::npos);

    // Mit einer Basis entstehen nur die neuen Typen.
    reflect::TypeRegistry base;
    world::register_types(base);
    const std::string app =
        script::generate_bindings(types, {.name_space = "Spiel", .base = &base});
    CHECK(app.find("namespace Spiel;") != std::string::npos);
    CHECK(app.find("using NGine;") != std::string::npos);
    CHECK(app.find("public struct BindingSample") != std::string::npos);
    CHECK(app.find("public struct Transform") == std::string::npos);
}

TEST_CASE("C# reads and changes the world without copies", "[script]") {
    auto& rt = runtime();
    reflect::TypeRegistry types;
    register_engine_types(types);
    // Die erzeugten Structs passen Byte für Byte zur Reflection, und keiner fehlt.
    const auto verified = rt.verify_bindings(types, true);
    INFO((verified ? std::string{} : verified.error()));
    REQUIRE(verified);

    world::World w{types};
    world::Entity first = w.create("Erste");
    first.set(world::Transform{});
    w.create("Zweite").set(world::Transform{.position = {1.0, 0.0, 0.0}});
    REQUIRE(rt.load_scripts(test_scripts()));
    rt.bind_world(&w);

    REQUIRE(rt.invoke("Tests.Scripts.WorldTests.MoveAll"));
    CHECK(first.get<world::Transform>()->position.x == 1.0);
    CHECK(w.entity(w.entity_ids().back()).get<world::Transform>()->position.x == 2.0);
    CHECK(capture()->contains("WorldTests: 2 Entities verschoben"));

    REQUIRE(rt.invoke("Tests.Scripts.WorldTests.EditOne"));
    CHECK(first.get<world::Transform>()->scale.x == 2.0f);
    CHECK(capture()->contains("WorldTests: Name Erste"));
    // Neue Komponenten und Entities erst nach dem Befehlspuffer.
    CHECK_FALSE(first.has<physics::RigidBody>());
    const usize before = w.entity_count();
    rt.apply_commands();
    REQUIRE(first.has<physics::RigidBody>());
    CHECK(first.get<physics::RigidBody>()->mass == 5.0f);
    // Felder, die C# nicht setzt, haben die Standardwerte aus C++.
    CHECK(first.get<physics::RigidBody>()->friction == 0.5f);
    CHECK(first.get<physics::RigidBody>()->allow_sleep);
    CHECK(w.entity_count() == before + 1);
    world::Entity created{};
    w.each<const world::Name>([&](world::Entity e, const world::Name& name) {
        if (name.value == "AusCSharp") {
            created = e;
        }
    });
    REQUIRE(created);
    CHECK(created.parent() == first);
    CHECK(created.get<world::Transform>()->position.z == 9.0);
    CHECK(created.get<world::Transform>()->rotation.w == 1.0f);

    REQUIRE(rt.invoke("Tests.Scripts.WorldTests.RemoveBodies"));
    CHECK(first.get<physics::RigidBody>()->mass == 6.0f);
    rt.apply_commands();
    CHECK_FALSE(first.has<physics::RigidBody>());

    // Fehler im Skript und unbekannte Methoden melden sich, statt abzustürzen.
    CHECK_FALSE(rt.invoke("Tests.Scripts.WorldTests.Throws"));
    CHECK(capture()->contains("Absichtlich"));
    CHECK_FALSE(rt.invoke("Tests.Scripts.WorldTests.GibtEsNicht"));

    rt.bind_world(nullptr);
    REQUIRE(rt.unload_scripts());
}

namespace {

template <typename T>
T& field(void* data, const reflect::TypeInfo& type, std::string_view name) {
    const reflect::Field* f = type.find_field(name);
    REQUIRE(f != nullptr);
    return *reinterpret_cast<T*>(static_cast<std::byte*>(data) + f->offset);
}

} // namespace

TEST_CASE("Script classes become components with fields and a life cycle", "[script]") {
    auto& rt = runtime();
    reflect::TypeRegistry types;
    register_engine_types(types);
    rt.use_types(&types);
    REQUIRE(rt.load_scripts(test_scripts()));

    // Jede Skript-Klasse ist ein Typ der Registry, sortiert nach Namen.
    REQUIRE(rt.script_types().size() == 3);
    CHECK(rt.script_types()[0]->name == "Bumper");
    CHECK(rt.script_types()[1]->name == "Spawner");
    CHECK(rt.script_types()[2]->name == "Spinner");
    const reflect::TypeInfo* spinner = types.find("Spinner");
    REQUIRE(spinner != nullptr);
    CHECK(spinner->fields.size() == 5); // ohne privates Feld
    CHECK(spinner->find_field("Speed")->range->max == 10.0);
    CHECK(spinner->find_field("Tint")->hint == reflect::Hint::Color);
    CHECK(spinner->find_field("Motion")->type == types.find("MotionType"));
    // Standardwerte aus den Feldinitialisierern in C#.
    void* prototype = reflect::TypeRegistry::composite_prototype(*spinner);
    CHECK(field<f32>(prototype, *spinner, "Speed") == 2.0f);
    CHECK(field<std::string>(prototype, *spinner, "Label") == "Kreisel");
    CHECK(field<math::Vec3>(prototype, *spinner, "Tint").y == 0.5f);
    CHECK(field<physics::MotionType>(prototype, *spinner, "Motion") ==
          physics::MotionType::Kinematic);

    world::World w{types};
    world::Entity entity = w.create("Kreisel");
    entity.set(world::Transform{});
    const world::ComponentId spinner_id = w.component(*spinner);
    field<std::string>(w.ensure(entity.id(), spinner_id), *spinner, "Label") = "Erster";
    world::Entity spawner = w.create("Spawner");
    (void)w.ensure(spawner.id(), w.component(*types.find("Spawner")));
    rt.bind_world(&w);

    const usize before = w.entity_count();
    REQUIRE(rt.run(script::ScriptPhase::Fixed, 0.5f));
    REQUIRE(rt.run(script::ScriptPhase::Fixed, 0.5f));
    REQUIRE(rt.run(script::ScriptPhase::Frame, 0.016f));
    CHECK(entity.get<world::Transform>()->position.x == 2.0);
    // Start hat über den Befehlspuffer Entities angelegt.
    CHECK(w.entity_count() == before + 2);
    // Öffentliche Felder kommen erst auf Wunsch in die Komponente.
    CHECK(field<i32>(w.get_mut(entity.id(), spinner_id), *spinner, "Calls") == 0);
    rt.store_fields();
    CHECK(field<i32>(w.get_mut(entity.id(), spinner_id), *spinner, "Calls") == 1102);
    // Werte aus der Komponente zurück ins Skript (Inspector, Undo).
    field<f32>(w.get_mut(entity.id(), spinner_id), *spinner, "Speed") = 0.0f;
    rt.load_fields();
    REQUIRE(rt.run(script::ScriptPhase::Fixed, 0.5f));
    CHECK(entity.get<world::Transform>()->position.x == 2.0);

    // Szenen speichern Skripte wie Komponenten.
    rt.store_fields();
    const std::string scene = world::save_scene(w);
    CHECK(scene.find("\"Spinner\"") != std::string::npos);
    CHECK(scene.find("\"Label\": \"Erster\"") != std::string::npos);

    // Komponente weg: Stop, privater Zustand bleibt bis dahin.
    w.remove(entity.id(), spinner_id);
    REQUIRE(rt.run(script::ScriptPhase::Fixed, 0.5f));
    CHECK(capture()->contains("Spinner Erster stoppt nach 1103 Aufrufen (0)"));

    // Neu laden mit gleichen Feldern behält die Typen.
    rt.bind_world(nullptr);
    const auto unloaded = rt.unload_scripts();
    REQUIRE(unloaded);
    CHECK(*unloaded); // Auch mit Skript-Komponenten wird der Kontext frei.
    REQUIRE(rt.load_scripts(test_scripts()));
    CHECK(types.find("Spinner") == spinner);

    REQUIRE(rt.unload_scripts());
    rt.use_types(nullptr);
}

TEST_CASE("Scripts read input, query physics and get physics events", "[script]") {
    auto& rt = runtime();
    reflect::TypeRegistry types;
    register_engine_types(types);
    rt.use_types(&types);
    REQUIRE(rt.load_scripts(test_scripts()));

    // Eingabe: Aktionen aus dem ActionState des Spiels.
    input::ActionState actions{input::ActionMap::standard()};
    actions.begin_frame();
    actions.handle(platform::Event{platform::event::KeyPressed{platform::Key::Space}});
    actions.handle(platform::Event{platform::event::KeyPressed{platform::Key::D}});
    actions.end_frame(0.016f);
    CHECK_FALSE(rt.invoke("Tests.Scripts.GameTests.CheckInput")); // ohne Eingabe nichts gedrückt
    rt.bind_input(&actions);
    REQUIRE(rt.invoke("Tests.Scripts.GameTests.CheckInput"));
    CHECK(capture()->contains("GameTests: Bewegung 1"));

    // Physik: Abfragen wie in C++.
    world::World w{types};
    world::Entity box = w.create("Kiste");
    box.set(world::Transform{.position = {0.0, 1.0, -5.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{.shape = physics::ShapeType::Box});
    world::Entity ball = w.create("Kugel");
    ball.set(world::Transform{.position = {5.0, 1.0, 0.0}});
    (void)w.ensure(ball.id(), w.component(*types.find("Bumper")));
    jobs::JobSystem jobs{{.worker_count = 1}};
    physics::PhysicsWorld physics{w, jobs};
    physics.sync();
    events::EventBus bus;
    rt.bind_world(&w);
    rt.bind_physics(&physics, &bus);
    REQUIRE(rt.invoke("Tests.Scripts.GameTests.CheckPhysics"));
    CHECK(capture()->contains(
        "GameTests: Treffer Kiste bei 4.50, ohne False, Kugel 4.00, Overlap 1"));

    // Ereignisse: gesammelt und vor FixedUpdate an die Skripte der beteiligten Entities.
    REQUIRE(rt.run(script::ScriptPhase::Fixed, 0.02f)); // Start
    bus.publish(physics::ContactBegan{.a = box.id(), .b = ball.id(), .normal = {0.0f, 1.0f, 0.0f}});
    bus.publish(physics::TriggerEntered{.sensor = box.id(), .other = ball.id()});
    bus.publish(physics::TriggerEntered{.sensor = ball.id(), .other = box.id()});
    REQUIRE(rt.run(script::ScriptPhase::Fixed, 0.02f));
    rt.store_fields();
    const reflect::TypeInfo* bumper = types.find("Bumper");
    void* data = w.get_mut(ball.id(), w.component(*bumper));
    CHECK(field<i32>(data, *bumper, "Touches") == 1);
    CHECK(field<i32>(data, *bumper, "Triggers") == 2);
    CHECK(field<f32>(data, *bumper, "LastNormalY") == -1.0f); // vom Skript zum anderen Körper
    // Ohne Bus kommen keine Ereignisse mehr an.
    rt.bind_physics(nullptr);
    bus.publish(physics::TriggerEntered{.sensor = box.id(), .other = ball.id()});
    REQUIRE(rt.run(script::ScriptPhase::Fixed, 0.02f));
    rt.store_fields();
    CHECK(field<i32>(w.get_mut(ball.id(), w.component(*bumper)), *bumper, "Triggers") == 2);

    rt.bind_input(nullptr);
    rt.bind_world(nullptr);
    REQUIRE(rt.unload_scripts());
    rt.use_types(nullptr);
}

TEST_CASE("Script values from a scene loaded before the scripts are kept and adopted", "[script]") {
    auto& rt = runtime();
    reflect::TypeRegistry types;
    register_engine_types(types);
    world::World w{types};
    // Szene vor den Skripten (Editor startet, Bau läuft noch): Werte bleiben als JSON.
    REQUIRE(world::load_scene(w, R"({
        "format": "ngine.scene", "version": 1,
        "entities": [ {"id": 1, "components": {
            "Name": {"value": "Alt"}, "Spinner": {"Speed": 5, "Label": "Aus der Szene"} } } ]
    })"));
    rt.bind_world(&w);
    rt.use_types(&types);
    REQUIRE(rt.load_scripts(test_scripts()));
    const reflect::TypeInfo* spinner = types.find("Spinner");
    REQUIRE(spinner != nullptr);
    usize found = 0;
    w.each<const world::Name>([&](world::Entity e, const world::Name&) {
        void* data = w.get_mut(e.id(), w.component(*spinner));
        REQUIRE(data != nullptr);
        CHECK(field<f32>(data, *spinner, "Speed") == 5.0f);
        CHECK(field<std::string>(data, *spinner, "Label") == "Aus der Szene");
        CHECK(field<i32>(data, *spinner, "Calls") == 0); // Standardwert für Fehlendes
        CHECK_FALSE(e.has<world::UnknownComponents>());
        ++found;
    });
    CHECK(found == 1);

    rt.bind_world(nullptr);
    REQUIRE(rt.unload_scripts());
    rt.use_types(nullptr);
}

TEST_CASE("Compiler messages become diagnostics with file and line", "[script]") {
    const std::string output =
        "/spiel/Laterne.cs(12,5): error CS1002: ; expected [/spiel/Skripte.csproj]\n"
        "/spiel/Laterne.cs(12,5): error CS1002: ; expected [/spiel/Skripte.csproj]\n"
        "C:\\spiel\\Tuer.cs(3,17): warning CS0168: Die Variable \"x\" ist deklariert, wird aber "
        "nie verwendet. [C:\\spiel\\Skripte.csproj]\n"
        "MSBUILD : error MSB1009: Projektdatei ist nicht vorhanden.\n"
        "Erstellen erfolgreich.\n";
    const auto diagnostics = script::parse_diagnostics(output);
    REQUIRE(diagnostics.size() == 3);
    CHECK(diagnostics[0].file == "/spiel/Laterne.cs");
    CHECK(diagnostics[0].line == 12);
    CHECK(diagnostics[0].column == 5);
    CHECK(diagnostics[0].code == "CS1002");
    CHECK(diagnostics[0].message == "; expected");
    CHECK(diagnostics[0].error);
    CHECK(diagnostics[1].file == "C:\\spiel\\Tuer.cs");
    CHECK_FALSE(diagnostics[1].error);
    CHECK(diagnostics[1].message.ends_with("nie verwendet."));
    CHECK(diagnostics[2].file == "MSBUILD");
    CHECK(diagnostics[2].line == 0);
    CHECK(diagnostics[2].code == "MSB1009");
}

namespace {

// Eigenes Skript-Projekt in einem leeren Ordner, wie assets/skripte.
struct ScriptProject {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                std::format("ngine_skripte_{}", std::random_device{}());

    ScriptProject() {
        std::filesystem::create_directories(dir / "quellen");
        write("quellen/Spiel.csproj", R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>enable</Nullable>
  </PropertyGroup>
  <ItemGroup>
    <Reference Include="NGine.Scripting">
      <HintPath>$(NgineScriptingDir)/NGine.Scripting.dll</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
</Project>
)");
    }
    ~ScriptProject() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
    ScriptProject(const ScriptProject&) = delete;
    ScriptProject& operator=(const ScriptProject&) = delete;

    void write(const std::string& name, const std::string& text) const {
        REQUIRE(filesystem::write_text((dir / name).string(), text));
    }
    [[nodiscard]] std::string project() const {
        return (dir / "quellen" / "Spiel.csproj").string();
    }
    [[nodiscard]] std::string cache() const {
        return (dir / "cache").string();
    }
};

std::string counter_script(std::string_view body, std::string_view extra = "") {
    return std::format(R"(using NGine;
public class Zaehler : Script
{{
    public int Schritt = 1;
    public int Summe;
    {}
    public override void FixedUpdate()
    {{
        {}
    }}
}}
)",
                       extra, body);
}

} // namespace

TEST_CASE("Scripts build in the background and reload with their field values", "[script]") {
    auto& rt = runtime();
    ScriptProject project;
    project.write("quellen/Zaehler.cs", counter_script("Summe += Schritt;"));

    reflect::TypeRegistry types;
    register_engine_types(types);
    world::World w{types};
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Simulation simulation{w, jobs};
    auto system = std::make_unique<script::ScriptSystem>(
        rt, types,
        script::ScriptSystemDesc{.project = project.project(), .cache_dir = project.cache()});
    system->start(true);
    const auto& status = system->status();
    REQUIRE(status.last_build.has_value());
    if (status.last_build->sdk_missing) {
        system.reset();
        if (dotnet_required()) {
            FAIL(".NET-SDK fehlt: " + status.last_build->output);
        }
        SKIP(".NET-SDK fehlt");
    }
    INFO(status.last_build->output);
    REQUIRE(status.last_build->ok);
    REQUIRE(status.generation == 1);
    CHECK(status.classes == std::vector<std::string>{"Zaehler"});
    CHECK(filesystem::exists(project.cache() + "/Spiel.dll"));
    CHECK(filesystem::exists(project.cache() + "/ngine.props"));

    const reflect::TypeInfo* counter = types.find("Zaehler");
    REQUIRE(counter != nullptr);
    world::Entity entity = w.create("Zaehler");
    (void)w.ensure(entity.id(), w.component(*counter));
    system->bind(&w);
    system->add_to(simulation);
    const auto step = [&] { simulation.advance(1.0 / 60.0 + 1e-6); };
    const auto value = [&](std::string_view name) -> i32& {
        return field<i32>(w.get_mut(entity.id(), w.component(*types.find("Zaehler"))),
                          *types.find("Zaehler"), name);
    };

    step();
    step();
    rt.store_fields();
    CHECK(value("Summe") == 2);
    // Wert aus dem Inspector.
    value("Schritt") = 5;
    rt.load_fields();
    step();
    rt.store_fields();
    CHECK(value("Summe") == 7);

    // Datei ändern: Neubau im Hintergrund, die Skripte laufen weiter, danach gilt der neue Code
    // mit den alten Werten; das neue Feld bekommt seinen Standardwert.
    project.write("quellen/Zaehler.cs",
                  counter_script("Summe += Schritt * 100;", "public float Neu = 2.5f;"));
    const auto wait_for = [&](auto&& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            system->update();
            step();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };
    wait_for([&] { return status.generation == 2; });
    REQUIRE(status.generation == 2);
    rt.store_fields();
    const i32 before = value("Summe");
    CHECK(value("Schritt") == 5);
    CHECK(before >= 7);
    const reflect::TypeInfo* changed = types.find("Zaehler");
    REQUIRE(changed != counter); // neue Felder, neuer Typ
    CHECK(field<f32>(w.get_mut(entity.id(), w.component(*changed)), *changed, "Neu") == 2.5f);
    step();
    rt.store_fields();
    CHECK(value("Summe") == before + 500);

    // Fehler im Skript: Meldung mit Datei und Zeile, die alten Skripte laufen weiter.
    project.write("quellen/Zaehler.cs",
                  counter_script("Summe += Schritt * 100", "public float Neu = 2.5f;"));
    wait_for([&] { return status.last_build && !status.last_build->ok; });
    REQUIRE(status.last_build.has_value());
    REQUIRE_FALSE(status.last_build->ok);
    CHECK(status.generation == 2);
    REQUIRE_FALSE(status.last_build->diagnostics.empty());
    const script::Diagnostic& error = status.last_build->diagnostics.front();
    CHECK(error.error);
    CHECK(error.file.ends_with("Zaehler.cs"));
    CHECK(error.line == 9);
    CHECK(error.code == "CS1002");
    rt.store_fields();
    const i32 still = value("Summe");
    step();
    rt.store_fields();
    CHECK(value("Summe") == still + 500);

    // Inaktiv (Editor ohne Spielmodus): Stop, danach läuft nichts.
    system->set_active(false);
    step();
    rt.store_fields();
    CHECK(value("Summe") == still + 500);

    system->bind(nullptr);
    system.reset();
    CHECK_FALSE(rt.scripts_loaded());
}

namespace {

// Stellvertreter für app::PlayerInput (apps/common): dieselben Namen, die Spielersteuerung.cs
// benutzt. Die Tests bauen ohne die Anwendungen.
enum class TestCameraView : u8 { FirstPerson, Shoulder };
struct TestPlayerInput {
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
    TestCameraView view = TestCameraView::FirstPerson;
    f32 eye_height = 1.6f;
    f32 dig_radius = 0.8f;
    f32 dig_reach = 6.0f;
    f32 look_speed = 0.0025f;
};

void register_player_stand_in(reflect::TypeRegistry& types) {
    types.add<TestCameraView>("CameraView")
        .value("FirstPerson", TestCameraView::FirstPerson)
        .value("Shoulder", TestCameraView::Shoulder);
    types.add<TestPlayerInput>("PlayerInput")
        .field("yaw", &TestPlayerInput::yaw)
        .field("pitch", &TestPlayerInput::pitch)
        .field("view", &TestPlayerInput::view)
        .field("eye_height", &TestPlayerInput::eye_height)
        .field("dig_radius", &TestPlayerInput::dig_radius)
        .field("dig_reach", &TestPlayerInput::dig_reach)
        .field("look_speed", &TestPlayerInput::look_speed);
}

} // namespace

TEST_CASE("Game scripts steer the player and switch the lantern", "[script]") {
    auto& rt = runtime();
    reflect::TypeRegistry types;
    register_engine_types(types);
    register_player_stand_in(types);
    const std::filesystem::path cache =
        std::filesystem::temp_directory_path() /
        std::format("ngine_spielskripte_{}", std::random_device{}());
    world::World w{types};
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Simulation simulation{w, jobs};
    input::ActionState actions{input::ActionMap::standard()};
    auto system = std::make_unique<script::ScriptSystem>(
        rt, types,
        script::ScriptSystemDesc{.project = NGINE_ASSET_SOURCE_DIR "/skripte/Skripte.csproj",
                                 .cache_dir = cache.string(),
                                 .watch = false});
    system->bind(&w, &actions);
    system->start(true);
    const auto& status = system->status();
    REQUIRE(status.last_build.has_value());
    if (status.last_build->sdk_missing) {
        system.reset();
        if (dotnet_required()) {
            FAIL(".NET-SDK fehlt: " + status.last_build->output);
        }
        SKIP(".NET-SDK fehlt");
    }
    INFO(status.last_build->output);
    REQUIRE(status.last_build->ok);
    CHECK(status.classes == std::vector<std::string>{"Laterne", "Spielersteuerung"});
    system->add_to(simulation);
    simulation.set_after_fixed_step([&] { actions.end_fixed_step(); });

    // Laterne mit Licht als Kind wie prefabs/laterne.prefab, Figur einen Meter daneben.
    world::Entity lantern = w.create("Laterne");
    lantern.set(world::Transform{});
    (void)w.ensure(lantern.id(), w.component(*types.find("Laterne")));
    world::Entity light = w.create("Leuchte");
    light.set(world::Transform{.position = {0.0, 3.0, 0.0}})
        .set(render::PointLight{.intensity = 6.0f});
    w.set_parent(light.id(), lantern.id());
    world::Entity player = w.create("Spielfigur");
    player.set(world::Transform{.position = {1.0, 0.0, 0.0}})
        .set(TestPlayerInput{})
        .set(physics::CharacterController{})
        .set(physics::CharacterMotion{});
    (void)w.ensure(player.id(), w.component(*types.find("Spielersteuerung")));

    const auto frame = [&](std::vector<platform::Event> events = {}) {
        actions.begin_frame();
        for (const platform::Event& event : events) {
            actions.handle(event);
        }
        actions.end_frame(1.0f / 60.0f);
        simulation.advance(1.0 / 60.0 + 1e-6);
    };
    frame();
    frame();
    const auto* point = light.get<render::PointLight>();
    CHECK(point->active);
    CHECK(point->intensity > 6.0f * 0.85f);
    CHECK(point->intensity < 6.0f * 1.15f);

    // F schaltet die Laterne in Reichweite aus.
    frame({platform::event::KeyPressed{platform::Key::F}});
    CHECK_FALSE(light.get<render::PointLight>()->active);
    frame({platform::event::KeyReleased{platform::Key::F}});
    CHECK(capture()->contains("Laterne: aus"));

    // W vorwärts, die Maus dreht um 100 Pixel nach rechts, Leertaste springt.
    frame({platform::event::KeyPressed{platform::Key::W},
           platform::event::MouseMoved{.x = 0.0f, .y = 0.0f, .dx = 100.0f},
           platform::event::KeyPressed{platform::Key::Space}});
    frame();
    const auto* input = player.get<TestPlayerInput>();
    CHECK(input->yaw == -0.25f);
    const auto* motion = player.get<physics::CharacterMotion>();
    // Nach rechts gedreht: vorwärts zeigt nach −z und etwas nach +x.
    CHECK(motion->move.z < -0.9f);
    CHECK(motion->move.x > 0.2f);
    CHECK(motion->jump);
    CHECK_FALSE(motion->run);

    // Ohne die Laterne kommt das ruhige Licht zurück (Stop).
    w.remove(lantern.id(), w.component(*types.find("Laterne")));
    frame();
    CHECK(light.get<render::PointLight>()->intensity == 6.0f);

    system->bind(nullptr);
    system.reset();
    std::error_code ignored;
    std::filesystem::remove_all(cache, ignored);
}
