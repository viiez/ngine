# 0008: Linux als Hauptplattform, Clang als Hauptcompiler

- **Status:** angenommen
- **Datum:** 2026-10-27

## Kontext

Entwickelt wird hauptsächlich auf Linux Mint; die starke GPU läuft unter Windows 11. Beide Plattformen
sollen gleichwertig unterstützt werden, Linux als „first class citizen“.

## Entscheidung

- Linux ist Hauptplattform, Windows baut in jedem PR mit (CI).
- Clang ist Hauptcompiler unter Linux. Unter Windows nutzen wir vorerst MSVC, weil dessen
  Modul-Unterstützung in CMake ausgereifter ist; clang-cl prüfen wir später.

## Konsequenzen

- Unter Linux Mint 22 werden ein neueres CMake und Clang nötig (siehe README).
- Plattformcode bleibt in der Plattformschicht.

## Erwogene Alternativen

- **Clang auf beiden Plattformen sofort:** ursprünglich geplant, für Windows aufgeschoben.
- **GCC als Linux-Compiler:** erst ab GCC 14 mit Modulen nutzbar.
