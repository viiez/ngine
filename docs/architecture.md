# Architektur

NGine ist in Schichten gebaut. Jede Schicht darf nur Schichten unter sich benutzen, nie umgekehrt.
Jedes System ist ein C++-Modul (`ngine.<system>`) mit eigenem CMake-Ziel (`ngine::<system>`).

## Schichten

```mermaid
flowchart TB
    Game["Spiel<br/>Gameplay-Code in C++ und C#"]
    Tools["Werkzeuge<br/>Editor, Asset-Pipeline"]
    World["Welt<br/>ECS, Szene, Voxel-Terrain, Scripting"]
    Runtime["Laufzeit-Systeme<br/>Rendering, Physik, Audio, Animation, Netzwerk"]
    Res["Ressourcen<br/>Ressourcen-Manager, Streaming"]
    Core["Core<br/>Typen, Log, Speicher, Jobs, Reflection"]
    Plat["Plattform und RHI<br/>Fenster, Eingabe, Dateien, Vulkan"]
    Game --> Tools --> World --> Runtime --> Res --> Core --> Plat
```

Die Pfeile zeigen „darf benutzen“. Die Werkzeuge stehen über der Welt, weil der Editor die Welt
bearbeitet; das Spiel nutzt die Werkzeuge nur im Editor-Build.

## Module heute

| Modul | CMake-Ziel | Inhalt | Status |
| --- | --- | --- | --- |
| `ngine.core` | `ngine::core` | Partitionen `types`, `log`, `version` | vorhanden |

Geplante Module, in der Reihenfolge der [Roadmap](roadmap.md): `ngine.platform`, `ngine.rhi` mit
`ngine.rhi.vulkan`, `ngine.jobs`, `ngine.world`, `ngine.voxel`, `ngine.script`, `ngine.net`.

## Regeln

- **Abhängigkeiten nur nach unten.** Ein Modul darf kein Modul einer höheren Schicht importieren.
- **Fremdbibliotheken nur in Adaptern** ([ADR 0009](adr/0009-third-party-behind-interfaces.md)). Ein
  Adapter bindet die Bibliothek im globalen Modulfragment ein und exportiert nur Engine-Typen.
- **Headless zuerst.** Alles unterhalb des Renderings muss ohne Fenster und GPU laufen, weil der
  dedizierte Server so startet ([ADR 0007](adr/0007-server-authoritative-multiplayer.md)).
- **Daten im ECS.** Spielzustand lebt in ECS-Komponenten, damit Speichern, Netzwerk und Editor ihn
  einheitlich behandeln ([ADR 0003](adr/0003-ecs-flecs.md)).
- **Plattformcode nur in der Plattformschicht.** Keine `#ifdef _WIN32` außerhalb von Adaptern.

## Verzeichnisse

| Pfad | Inhalt |
| --- | --- |
| `engine/<system>/` | Ein Engine-System |
| `apps/sandbox/` | Testanwendung, an der jede Iteration sichtbar wird |
| `apps/editor/` | Editor (ab R2) |
| `tests/<system>/` | Catch2-Tests, gespiegelt zur Engine |
| `cmake/` | Gemeinsame CMake-Funktionen |
| `docs/` | Diese Dokumentation |
