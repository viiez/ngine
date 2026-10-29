# 0002: C++23 mit Modulen, CMake und vcpkg

- **Status:** angenommen
- **Datum:** 2026-10-27

## Kontext

Die Engine soll modernes C++ nutzen. C++-Module versprechen schnellere Builds und klare Grenzen, sind
in Game Engines aber noch kein Standard, und die Werkzeuge reifen erst.

## Entscheidung

- C++23, eigener Code als Module (`ngine.<system>`, Partitionen `ngine.<system>:<teil>`).
- Standard- und Fremd-Header im globalen Modulfragment; noch kein `import std;`.
- CMake ab 3.30 mit Ninja und Presets; Pakete über vcpkg im Manifest-Modus.

## Konsequenzen

- Iteration A1 hat bestätigt: Clang und MSVC bauen Module mit CMake zuverlässig.
- Iteration A2: Clang 18 kann `std::expected` aus libstdc++ nicht nutzen; Mindestversion ist daher
  Clang 20 (in den Paketquellen von Ubuntu 24.04 und Linux Mint 22 enthalten).
- GCC wird erst ab Version 14 unterstützt; das GCC 13 von Ubuntu 24.04 und Linux Mint 22 reicht nicht.
- IDE-Unterstützung (clangd, IntelliSense) für Module ist noch lückenhaft.
- Falls Module bremsen, bleibt der Rückweg zu Headern offen.

## Erwogene Alternativen

- **Klassische Header:** bewährt, aber langsamere Builds und keine erzwungenen Grenzen.
- **Conan oder FetchContent statt vcpkg:** siehe [Fremdbibliotheken](../third-party.md).
