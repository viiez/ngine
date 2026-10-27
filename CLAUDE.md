# NGine

NGine (Arbeitstitel) ist eine 3D-Engine in C++23 mit Vulkan, glattem Voxel-Terrain, C#-Scripting,
eigenem Editor und Multiplayer. Lernprojekt und Grundlage für ein Survival-RPG mit Horror-Elementen.
Vision, Entscheidungen und Reifegradmodell stehen im Planungsdokument „Engine-Vision“ im Claude-Projekt.

## Bauen und testen

Voraussetzungen: CMake 3.30 oder neuer, Ninja, `VCPKG_ROOT` zeigt auf eine vcpkg-Installation.

- Linux: Clang 18 oder neuer (`clang-tools` für `clang-scan-deps`).
- Windows: MSVC (Visual Studio 2022) aus einer Developer-Konsole.
- GCC wird erst ab Version 14 unterstützt (C++-Module).

```sh
cmake --preset linux-clang-debug        # bzw. windows-msvc-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
./build/linux-clang-debug/apps/sandbox/ngine_sandbox --headless
```

## Aufbau

- `engine/<system>/` ein Engine-System, ein CMake-Ziel `ngine_<system>` mit Alias `ngine::<system>`.
- `apps/sandbox/` Testanwendung, an der jede Stufe sichtbar wird.
- `tests/<system>/` Catch2-Tests, gespiegelt zur Engine-Struktur.
- `cmake/` gemeinsame CMake-Funktionen, z. B. `ngine_set_compiler_options()`.

## Konventionen

- C++23, eigener Code als C++-Module. Modulname `ngine.<system>`, Partitionen `ngine.<system>:<teil>`.
  Die primäre Schnittstelle exportiert alle Partitionen.
- Header (Standardbibliothek, Fremdbibliotheken) nur im globalen Modulfragment (`module;` … ).
  Kein `import std;`, bis alle Zielcompiler es zuverlässig können.
- Fremdbibliotheken nur hinter einer Engine-Schnittstelle. Außerhalb des Adapters kennt kein Code die
  Bibliothek direkt, damit sie austauschbar bleibt.
- Linux ist Hauptplattform, Windows muss immer mitbauen. Plattformcode nur in Plattform-Adaptern.
- Namen: Typen `PascalCase`, Funktionen und Variablen `snake_case`, Konstanten `snake_case`,
  Namespaces klein (`ngine::log`). Zahlentypen aus `ngine.core:types` (`u32`, `f32`, …).
- Kommentare und Log-Ausgaben auf Deutsch, Bezeichner auf Englisch.
- Keine nackten `new`/`delete`. Besitz über `std::unique_ptr`/`std::shared_ptr` oder eigene Handles.
- Keine Warnungen: CI baut mit `NGINE_WARNINGS_AS_ERRORS=ON`.
- Formatierung mit `.clang-format`.
- Jede Änderung mit Tests, soweit ohne GPU testbar. Die Engine muss ohne Fenster und GPU starten
  können (`--headless`), weil der dedizierte Server so läuft.
