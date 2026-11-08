# NGine

NGine (Arbeitstitel) ist eine 3D-Engine in C++23 mit Vulkan, glattem Voxel-Terrain, C#-Scripting,
eigenem Editor und Multiplayer. Lernprojekt und Grundlage für ein Survival-RPG mit Horror-Elementen.
Vision, Prozess, Roadmap, Feature-Katalog und Entscheidungen stehen in [`docs/`](docs/README.md).

## Dokumentation (Docs as Code)

- `docs/` ist die verbindliche Quelle. Doku ändert sich im selben PR wie der Code.
- Wir arbeiten nach einem vereinfachten Unified Process ([docs/process.md](docs/process.md)):
  Phasen, Iterationen mit Plan und Review unter `docs/iterations/`, Definition of Done.
- Feature fertig oder neu → Status und ID in [docs/features.md](docs/features.md) pflegen.
- Neues Modul oder geänderte Schnittstelle → [docs/architecture.md](docs/architecture.md).
- Neue oder ersetzte Bibliothek → [docs/third-party.md](docs/third-party.md).
- Schwer umkehrbare Entscheidung → neues ADR unter `docs/adr/` (Vorlage `template.md`) und Eintrag
  in der ADR-Liste.
- Diagramme als Mermaid in Markdown.

## Bauen und testen

Voraussetzungen: CMake 3.28 oder neuer, Ninja, `VCPKG_ROOT` zeigt auf eine vcpkg-Installation.
Apt-Pakete für Linux stehen in der README.

- Linux: Baseline ist Ubuntu 24.04, alles kommt aus dessen Paketquellen (ADR 0017): Clang 20
  (`clang-tools-20` für `clang-scan-deps`, `libstdc++-14-dev`), CMake 3.28. Mindestversion ist
  Clang 20; Clang 18 kann `std::expected` aus libstdc++ nicht übersetzen. Code darf nichts brauchen,
  was diese Baseline nicht kann.
- Windows: MSVC (aktuelles Visual Studio) aus einer Developer-Konsole.
- GCC wird erst ab Version 14 unterstützt (C++-Module).

```sh
cmake --preset linux-clang-debug        # bzw. windows-msvc-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
./build/linux-clang-debug/apps/sandbox/ngine_sandbox --headless
```

Job-System, Allokatoren und Systeme der Welt prüft ThreadSanitizer (Preset `linux-clang-tsan`, Tests
`[jobs],[memory],[world]`).
Netzwerktests (`[net]`) nutzen nur localhost mit zufälligen Ports.
GPU-Tests (`[gpu]`) laufen in der CI über lavapipe; ohne Vulkan-Treiber werden sie übersprungen.
Vulkan: Pflicht ist 1.3 mit Bindless; Mesh Shader und Raytracing sind optionale Stufen
(`Capabilities::features`). Wer eine Stufe nutzt, baut einen Rückfallpfad und testet ihn mit
`disabled_features` bzw. `ngine_sandbox --baseline`.
`NGINE_WITH_WINDOW=OFF` baut ohne SDL3. C#-Scripting braucht zum Bauen das .NET-SDK 10
(`dotnet-sdk-10.0` aus Ubuntu 24.04); ohne SDK schaltet sich `NGINE_WITH_DOTNET` ab und `[script]`-Tests
werden übersprungen (in der CI mit `NGINE_REQUIRE_DOTNET` nicht). Shader sind Slang-Dateien und werden beim Bauen mit
`slangc` zu SPIR-V kompiliert (`ngine_compile_shader()` in `cmake/NgineShaders.cmake`).

## Aufbau

- `engine/<system>/` ein Engine-System, ein CMake-Ziel `ngine_<system>` mit Alias `ngine::<system>`.
- `apps/sandbox/` Testanwendung, an der jede Iteration sichtbar wird.
- `apps/editor/` Editor (ab R2.1, Dear ImGui hinter `ngine.editor_ui`, ADR 0029; Logik ohne UI in
  `ngine.edit`, ADR 0030); `apps/common/` Code, den Sandbox und Editor teilen.
- `assets/` Quell-Assets mit je einer `.meta`-Datei (Asset-ID, ADR 0033); `.meta`-Dateien gehören
  ins Repository und wandern beim Verschieben mit.
- `tests/<system>/` Catch2-Tests, gespiegelt zur Engine-Struktur.
- `cmake/` gemeinsame CMake-Funktionen, z. B. `ngine_set_compiler_options()`.

## Konventionen

- C++23, eigener Code als C++-Module. Modulname `ngine.<system>`, Partitionen `ngine.<system>:<teil>`.
  Die primäre Schnittstelle exportiert alle Partitionen.
- Header (Standardbibliothek, Fremdbibliotheken) nur im globalen Modulfragment (`module;` … ).
  Kein `import std;`, bis alle Zielcompiler es zuverlässig können.
- Schwere Header der Standardbibliothek (z. B. `<mutex>`, `<thread>`) nicht in Schnittstellen von
  Modulen, die viele importieren, sondern hinter einem Pimpl: MSVC bricht sonst beim Import ab (R2.1).
- Einzige eigene Header-Datei: `<ngine/profile.hpp>` mit den Profiling-Makros, weil Module keine
  Makros exportieren (ADR 0016). Sie enthält nur Makros.
- Komponenten sind Structs, die in der Reflection registriert sind (`types.add<T>("T").field(...)`,
  ADR 0018). Systeme ändern nur ihre Komponenten; Strukturänderungen kommen nach `Schedule::run()`.
- Fremdbibliotheken nur hinter einer Engine-Schnittstelle. Außerhalb des Adapters kennt kein Code die
  Bibliothek direkt, damit sie austauschbar bleibt.
- Aktuelle Versionen von Werkzeugen und Bibliotheken nutzen, wenn nichts dagegen spricht
  (Compiler, CMake, vcpkg-Baseline, GitHub Actions). Gründe für ältere Versionen ins ADR schreiben.
- Linux ist Hauptplattform, Windows muss immer mitbauen. Plattformcode nur in Plattform-Adaptern.
- Namen: Typen `PascalCase`, Funktionen und Variablen `snake_case`, Konstanten `snake_case`,
  Namespaces klein (`ngine::log`). Zahlentypen aus `ngine.core:types` (`u32`, `f32`, …).
- Kommentare und Log-Ausgaben auf Deutsch, Bezeichner auf Englisch.
- Keine nackten `new`/`delete`. Besitz über `std::unique_ptr`/`std::shared_ptr` oder eigene Handles.
- Keine Warnungen: CI baut mit `NGINE_WARNINGS_AS_ERRORS=ON`.
- Formatierung mit `.clang-format`.
- Testnamen (`TEST_CASE`) nur mit ASCII-Zeichen: ctest gibt sie unter Windows sonst falsch kodiert weiter.
- Jede Änderung mit Tests, soweit ohne GPU testbar. Die Engine muss ohne Fenster und GPU starten
  können (`--headless`), weil der dedizierte Server so läuft.
