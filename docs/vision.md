# Vision

NGine (Arbeitstitel) ist eine moderne 3D-Engine in C++23 mit Vulkan, glattem Voxel-Terrain im Stil von
Enshrouded, C#-Scripting, eigenem Editor und Multiplayer. Sie ist Lernprojekt und Grundlage für ein
eigenes Spiel. Mit Unreal oder Unity konkurrieren will sie nicht; sie soll aber moderne und innovative
Techniken nutzen und das Beste anderer Engines kombinieren, wo es Sinn ergibt.

Dieses Dokument ist lebendig: Wir prüfen es bei jedem Review (siehe [Prozess](process.md)) und passen es an.

## Spielidee

Ein Survival-RPG mit faszinierender Natur, die auch Horror-Elemente bietet. Inspiration: Enshrouded,
Valheim, Terraria, Superliminal und The Mortuary Assistant. Die Idee ist noch grob und wird parallel zur
Engine ausgearbeitet.

Daraus folgen Anforderungen an die Engine, die früh wichtig werden:

- große, veränderbare Welt (Voxel-Terrain, Bauen, Abbauen)
- viel Vegetation, Tag-Nacht-Wechsel, Wetter, Nebel und Atmosphäre
- stimmungsvolles Licht und Schatten, Dunkelheit als Spielelement
- räumlicher Klang
- Koop-Multiplayer

## Leitprinzipien

1. **Modern statt historisch:** aktuelle Techniken (Vulkan 1.3 mit Dynamic Rendering und Bindless,
   Render-Graph, Job-System, datenorientiertes Design), keine Altlasten.
2. **Das Beste kombinieren:** bewährte Ideen gezielt übernehmen, z. B. Reflection und RHI von Unreal,
   die C#-Trennung von Unity, 64-Bit-Weltkoordinaten von Unigine und die veränderbare Voxel-Welt von
   Enshrouded.
3. **Schritt für Schritt:** jede Iteration endet mit etwas Sichtbarem und Lauffähigem; kein System auf
   Vorrat.
4. **Offen für Neues:** Systeme kommunizieren über schmale Schnittstellen; ein neues Feature kommt als
   Modul dazu, ohne Bestehendes umzubauen.
5. **Austauschbar:** jede Fremdbibliothek steckt hinter einer Engine-Schnittstelle
   ([ADR 0009](adr/0009-third-party-behind-interfaces.md)).
6. **Aktuelle Werkzeuge:** Compiler, Build-Werkzeuge und Bibliotheken in aktueller Version, solange
   nichts dagegen spricht ([ADR 0008](adr/0008-linux-first-clang.md)).

## Ziele und Nicht-Ziele

| Ziele | Nicht-Ziele |
| --- | --- |
| Verstehen, wie eine moderne Engine funktioniert, durch eigenes Bauen | Konkurrenz zu kommerziellen Engines |
| Eine Engine, die das eigene Spiel tragen kann | Unterstützung aller Genres und Plattformen |
| Saubere, erweiterbare Architektur mit guter Doku | Alles selbst schreiben: Physik, Audio u. Ä. kommen als Bibliothek |
| Läuft flüssig auch auf schwacher Hardware (integrierte Grafik) | Fotorealismus um jeden Preis |

## Rahmenbedingungen

| Thema | Festlegung | Details |
| --- | --- | --- |
| Sprache und Build | C++23 mit Modulen, CMake, vcpkg | [ADR 0002](adr/0002-cpp23-modules-cmake-vcpkg.md) |
| Plattformen | Linux zuerst (Linux Mint), Windows gleichwertig | [ADR 0008](adr/0008-linux-first-clang.md) |
| Grafik-API | Vulkan 1.3 hinter einer RHI, später weitere APIs | [ADR 0004](adr/0004-vulkan-rhi-feature-tiers.md) |
| Objektmodell | ECS mit flecs, Entity-API darüber | [ADR 0003](adr/0003-ecs-flecs.md) |
| Terrain | Glattes Voxel-Terrain, alles andere Meshes | [ADR 0005](adr/0005-smooth-voxel-terrain.md) |
| Welten | 64-Bit-Weltpositionen, Rendering relativ zur Kamera | [ADR 0012](adr/0012-64bit-world-coordinates.md) |
| Scripting | C# über eingebettetes .NET | [ADR 0006](adr/0006-csharp-scripting-dotnet.md) |
| Multiplayer | Server-autoritativ, Listen- und dedizierter Server | [ADR 0007](adr/0007-server-authoritative-multiplayer.md) |
| Editor | Eigener Editor von Anfang an, zuerst mit Dear ImGui | [Feature-Katalog](features.md#editor) |

## Hardware zum Testen

| Gerät | Rolle | Grafik |
| --- | --- | --- |
| Notebook mit Linux Mint 22 | Hauptentwicklung | Intel-UHD-Grafik (Core i7, 8. Gen.): Vulkan 1.3 über Mesa, keine Mesh Shader, kein Raytracing |
| PC mit Windows 11 | Leistungstests, Nsight | NVIDIA RTX 3060 Ti: Mesh Shader und Raytracing verfügbar |

Die Basis der Engine muss auf der Intel-Grafik laufen; Mesh Shader und Raytracing sind optionale
Feature-Stufen.
