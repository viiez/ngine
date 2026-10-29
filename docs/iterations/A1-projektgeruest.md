# A1: Projektgerüst

- **Phase:** Ausarbeitung
- **Status:** abgeschlossen
- **Pull Requests:** [#1](https://github.com/viiez/ngine/pull/1)

## Plan

**Ziel:** Ein Projekt, das auf Linux und Windows baut, testet und C++-Module nutzt.

**Features:** CORE-01, CORE-02, CORE-03, PLAT-03 (Anfang), TOOL-01, TOOL-02, TOOL-03

**Risiken:** C++-Module mit CMake und den Zielcompilern funktionieren nicht zuverlässig.

**Abnahme:** CI grün auf Linux und Windows, Tests laufen, Sandbox startet mit `--headless`.

## Review

**Ergebnis:** CMake mit Presets, `ngine.core` als Modul mit Logging, Sandbox, Catch2-Tests, CI. CI war
auf beiden Plattformen beim ersten Lauf grün.

**Erkenntnisse:**

- Clang 18 und MSVC bauen Module mit CMake ohne Probleme.
- GCC 13 (Ubuntu 24.04, Linux Mint 22) kann Module mit CMake nicht; GCC erst ab 14.
- Ein eigener Logger auf `std::format` ersetzt spdlog mühelos.
- Das CMake von Ubuntu 24.04 ist mit 3.28 zu alt.

**Anpassungen:** ADR 0002, 0008 und 0010; Windows-Preset vorerst mit MSVC statt Clang.
