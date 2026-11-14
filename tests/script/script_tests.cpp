#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

import ngine.core;
import ngine.filesystem;
import ngine.math;
import ngine.reflect;
import ngine.world;
import ngine.physics;
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
    if (!instance) {
        capture();
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
    // Greeter und Mover (D2), Spawner und Spinner (Skript-Komponenten), nicht die abstrakte Klasse.
    CHECK(*count == 4);

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
    CHECK(*again == 4);
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
    REQUIRE(rt.script_types().size() == 2);
    CHECK(rt.script_types()[0]->name == "Spawner");
    CHECK(rt.script_types()[1]->name == "Spinner");
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
