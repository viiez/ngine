# Vision

NGine (Arbeitstitel) ist eine moderne 3D-Engine in C++23 mit Vulkan, glattem Voxel-Terrain im Stil von
Enshrouded, C#-Scripting, eigenem Editor und Multiplayer. Sie ist Lernprojekt und Grundlage für ein
eigenes Spiel. Mit Unreal oder Unity können wir nicht konkurrieren, weil hinter ihnen viel mehr
Menschen und Erfahrung stehen. Das ist eine realistische Einschätzung, keine Einschränkung des
Anspruchs: NGine soll moderne und innovative Techniken nutzen, das Beste anderer Engines kombinieren
und so schnell sein, wie wir es hinbekommen.

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
   Render-Graph, Job-System, datenorientiertes Design), keine Altlasten wie Render Passes. Mesh Shader,
   Raytracing und Vulkan 1.4 nutzen wir, wo die GPU sie kann; ohne sie bleibt die Engine lauffähig
   ([ADR 0017](adr/0017-platform-baseline-feature-tiers.md)).
2. **Das Beste kombinieren:** bewährte Ideen gezielt übernehmen, z. B. Reflection und RHI von Unreal,
   die C#-Trennung von Unity, 64-Bit-Weltkoordinaten von Unigine und die veränderbare Voxel-Welt von
   Enshrouded.
3. **Schritt für Schritt:** jede Iteration endet mit etwas Sichtbarem und Lauffähigem; kein System auf
   Vorrat.
4. **Offen für Neues:** Systeme kommunizieren über schmale Schnittstellen; ein neues Feature kommt als
   Modul dazu, ohne Bestehendes umzubauen.
5. **Austauschbar:** jede Fremdbibliothek steckt hinter einer Engine-Schnittstelle
   ([ADR 0009](adr/0009-third-party-behind-interfaces.md)).
6. **Seriös und performant:** Wir tun alles, was wir können, um die Engine schnell zu machen:
   datenorientiert, parallel, GPU-getrieben. Leistung messen wir von Anfang an, Optimierung ist kein
   Nachgedanke.
7. **Sauber statt schnell fertig:** Architektur und Design prüfen wir regelmäßig; technische Schulden
   beseitigen wir, solange sie klein sind ([Prozess](process.md#technische-schulden)).
8. **Aktuelle Werkzeuge:** Compiler, Build-Werkzeuge und Bibliotheken in aktueller Version, solange
   nichts dagegen spricht. Unter Linux muss alles mit den Paketen von Ubuntu 24.04 bauen
   ([ADR 0017](adr/0017-platform-baseline-feature-tiers.md)).

## Ziele und Nicht-Ziele

| Ziele | Nicht-Ziele |
| --- | --- |
| Verstehen, wie eine moderne Engine funktioniert, durch eigenes Bauen | Funktionsumfang kommerzieller Engines: dafür fehlen uns Personal und Erfahrung, nicht der Anspruch |
| Eine Engine, die das eigene Spiel tragen kann | Unterstützung aller Genres und Plattformen |
| Saubere, erweiterbare Architektur mit guter Doku | Alles selbst schreiben: Physik, Audio u. Ä. kommen als Bibliothek |
| Hohe Leistung auf moderner Hardware durch GPU-getriebenes Rendering | Fotorealismus um jeden Preis |
| Linux und Windows gleichwertig | Unterstützung von GPUs ohne Vulkan 1.3 |
| Läuft auch auf integrierter Grafik, mit weniger Effekten | Für jede optionale Stufe denselben Funktionsumfang auf schwacher Hardware |

## Rahmenbedingungen

| Thema | Festlegung | Details |
| --- | --- | --- |
| Sprache und Build | C++23 mit Modulen, CMake, vcpkg | [ADR 0002](adr/0002-cpp23-modules-cmake-vcpkg.md) |
| Plattformen | Linux zuerst (Linux Mint), Windows gleichwertig | [ADR 0008](adr/0008-linux-first-clang.md) |
| Grafik-API | Vulkan hinter einer RHI, später weitere APIs | [ADR 0004](adr/0004-vulkan-rhi-feature-tiers.md) |
| Linux-Baseline | Ubuntu 24.04 LTS (auch Linux Mint 22), Mesa 24 oder neuer | [ADR 0017](adr/0017-platform-baseline-feature-tiers.md) |
| Mindest-GPU | Vulkan 1.3 mit Bindless; Mesh Shader, Raytracing und Vulkan 1.4 optional | [ADR 0017](adr/0017-platform-baseline-feature-tiers.md) |
| Objektmodell | ECS mit flecs, Entity-API darüber | [ADR 0003](adr/0003-ecs-flecs.md) |
| Terrain | Glattes Voxel-Terrain, alles andere Meshes | [ADR 0005](adr/0005-smooth-voxel-terrain.md) |
| Welten | 64-Bit-Weltpositionen, Rendering relativ zur Kamera | [ADR 0012](adr/0012-64bit-world-coordinates.md) |
| Scripting | C# über eingebettetes .NET | [ADR 0006](adr/0006-csharp-scripting-dotnet.md) |
| Multiplayer | Server-autoritativ, Listen- und dedizierter Server | [ADR 0007](adr/0007-server-authoritative-multiplayer.md) |
| Editor | Eigener Editor von Anfang an, zuerst mit Dear ImGui | [Feature-Katalog](features.md#editor) |

## Hardware zum Testen

| Gerät | Rolle | Grafik |
| --- | --- | --- |
| PC mit Windows 11 | Tests auf echter Hardware, Leistung, Nsight | NVIDIA RTX 3060 Ti: alle optionalen Stufen |
| Notebook mit Linux Mint 22 | Programmieren, Bauen, Tests der Mindestanforderung | Intel-UHD-Grafik: Mindestanforderung ohne optionale Stufen |
| CI (GitHub Actions) | Automatische Tests unter Linux | lavapipe: Vulkan 1.4, Mesh Shader und Raytracing in Software; zusätzlich ein Lauf ohne Stufen |

Die RTX 3060 Ti zeigt, was mit allen Stufen möglich ist; das Notebook prüft, dass die Engine auf der
Mindestanforderung läuft ([ADR 0017](adr/0017-platform-baseline-feature-tiers.md)). Damit Linux auch
auf starker Hardware getestet wird, ist mittelfristig Linux auf dem PC mit der RTX 3060 Ti geplant.
