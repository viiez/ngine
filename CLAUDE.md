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

Voraussetzungen: CMake 3.30 oder neuer, Ninja, `VCPKG_ROOT` zeigt auf eine vcpkg-Installation.
Apt-Pakete für Linux stehen in der README.

- Linux: Clang 23 aus apt.llvm.org (`clang-tools-23` für `clang-scan-deps`, `libstdc++-14-dev`).
  Mindestversion ist Clang 20; Clang 18 kann `std::expected` aus libstdc++ nicht übersetzen.
- Windows: MSVC (aktuelles Visual Studio) aus einer Developer-Konsole.
- GCC wird erst ab Version 14 unterstützt (C++-Module).

```sh
cmake --preset linux-clang-debug        # bzw. windows-msvc-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
./build/linux-clang-debug/apps/sandbox/ngine_sandbox --headless
```

GPU-Tests (`[gpu]`) laufen in der CI über lavapipe; ohne Vulkan-Treiber werden sie übersprungen.
`NGINE_WITH_WINDOW=OFF` baut ohne SDL3. Shader sind Slang-Dateien und werden beim Bauen mit
`slangc` zu SPIR-V kompiliert (`ngine_compile_shader()` in `cmake/NgineShaders.cmake`).

## Aufbau

- `engine/<system>/` ein Engine-System, ein CMake-Ziel `ngine_<system>` mit Alias `ngine::<system>`.
- `apps/sandbox/` Testanwendung, an der jede Iteration sichtbar wird.
- `tests/<system>/` Catch2-Tests, gespiegelt zur Engine-Struktur.
- `cmake/` gemeinsame CMake-Funktionen, z. B. `ngine_set_compiler_options()`.

## Konventionen

- C++23, eigener Code als C++-Module. Modulname `ngine.<system>`, Partitionen `ngine.<system>:<teil>`.
  Die primäre Schnittstelle exportiert alle Partitionen.
- Header (Standardbibliothek, Fremdbibliotheken) nur im globalen Modulfragment (`module;` … ).
  Kein `import std;`, bis alle Zielcompiler es zuverlässig können.
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
