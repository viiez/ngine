#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

import ngine.core;
import ngine.filesystem;
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
    CHECK(*count == 2); // Greeter und Mover, nicht die abstrakte Klasse

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
    CHECK(*again == 2);
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
