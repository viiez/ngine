# 0008: Linux als Hauptplattform, Clang als Hauptcompiler

- **Status:** angenommen
- **Datum:** 2026-10-27

## Kontext

Entwickelt wird hauptsächlich auf Linux Mint; die starke GPU läuft unter Windows 11. Beide Plattformen
sollen gleichwertig unterstützt werden, Linux als „first class citizen“.

## Entscheidung

- Linux ist Hauptplattform, Windows baut in jedem PR mit (CI).
- Clang (ab Version 20) ist Hauptcompiler unter Linux. Unter Windows nutzen wir vorerst MSVC, weil dessen
  Modul-Unterstützung in CMake ausgereifter ist; clang-cl prüfen wir später.

## Konsequenzen

- Unter Linux Mint 22 werden ein neueres CMake und Clang 20 nötig (siehe README). Clang 18 scheitert an
  `std::expected` aus libstdc++.
- Plattformcode bleibt in der Plattformschicht.

## Erwogene Alternativen

- **Clang auf beiden Plattformen sofort:** ursprünglich geplant, für Windows aufgeschoben.
- **GCC als Linux-Compiler:** erst ab GCC 14 mit Modulen nutzbar.
