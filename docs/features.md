# Feature-Katalog

Übersicht aller Features, gruppiert nach Engine-Komponenten. Der Katalog ist Ausgangspunkt für
Planung und Brainstorming und wächst ständig. Iterationspläne verweisen auf die IDs.

**Status:** `fertig` · `in Arbeit` · `geplant` (in der Roadmap eingeordnet) · `Idee` (noch nicht
eingeordnet). **Wann** nennt Iteration oder Reifegrad aus der [Roadmap](roadmap.md).

Neue Ideen kommen zuerst in den [Ideen-Parkplatz](#ideen-parkplatz) und wandern bei einem Review in
eine Komponente.

## Inhalt

[Core](#core) · [Plattform](#plattform) · [Rendering](#rendering) · [Welt und ECS](#welt-und-ecs) ·
[Voxel-Terrain](#voxel-terrain) · [Natur und Atmosphäre](#natur-und-atmosphäre) ·
[Physik](#physik) · [Animation](#animation) · [Audio](#audio) · [Scripting](#scripting) ·
[Netzwerk](#netzwerk) · [Assets](#assets) · [Editor](#editor) · [Spiel-UI](#spiel-ui) ·
[KI und Navigation](#ki-und-navigation) · [Werkzeuge und Build](#werkzeuge-und-build) ·
[Ideen-Parkplatz](#ideen-parkplatz)

## Core

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| CORE-01 | Zahlentypen | `u32`, `f32` usw. aus `ngine.core:types` | A1 | fertig |
| CORE-02 | Logging | Stufen, austauschbare Sinks, Konsole | A1 | fertig |
| CORE-03 | Versionsinfo | Engine-Name und Version aus CMake | A1 | fertig |
| CORE-04 | Prüfungen | `ngine::verify()` bricht mit Meldung, Datei und Zeile ab | A2 | fertig |
| CORE-05 | Job-System | Tasks mit Abhängigkeiten auf allen Kernen: Work-Stealing, `Counter`, `TaskGraph`, `parallel_for` ([ADR 0015](adr/0015-own-job-system.md)) | A3 | fertig |
| CORE-06 | Speicher-Allokatoren | Frame-, Pool- und Linear-Allokatoren mit Budget-Tracking (`ngine.memory`, `std::pmr`) | A3 | fertig |
| CORE-07 | Profiling | Tracy-Zonen für CPU (A3) und GPU (mit dem Render-Graph in R1), Thread-Namen, Plots | A3 / R1 | in Arbeit |
| CORE-08 | Reflection | Typinformationen für Editor, Speichern, Netzwerk und C# | A4 | geplant |
| CORE-09 | Serialisierung | Binär und lesbar (z. B. JSON) über Reflection | A4 | geplant |
| CORE-10 | Mathematik | Vektoren, Matrizen, Quaternionen; zuerst GLM, später eigene SIMD-Typen | R1 | geplant |
| CORE-11 | Ereignisse | Lose gekoppelte Nachrichten zwischen Systemen | R2 | geplant |
| CORE-12 | Log-Datei | Sink, der in eine Datei schreibt | R2 | geplant |

## Plattform

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| PLAT-01 | Fenster | Fenster über SDL3, Wayland und X11 unter Linux | A2 | fertig |
| PLAT-02 | Eingabe | Tastatur, Maus, Gamepad, Abbildung auf Aktionen; bisher nur Schließen und Escape | A2 / R2 | in Arbeit |
| PLAT-03 | Headless-Start | Engine läuft ohne Fenster und GPU (Server, Tests); Build ohne SDL3 mit `NGINE_WITH_WINDOW=OFF` | A1 / A2 | fertig |
| PLAT-04 | Dateisystem | Pfade, Dateien lesen und schreiben, Benutzerordner | A3 | geplant |
| PLAT-05 | Virtuelles Dateisystem | Archive, Mounts, einheitliche Pfade | R2 | Idee |
| PLAT-06 | Hot Reload von Dateien | Änderungen an Shadern und Assets zur Laufzeit erkennen | R1 | geplant |

## Rendering

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| REND-01 | RHI | Schmale Schnittstelle über Vulkan; später D3D12 und Metal ([ADR 0013](adr/0013-rhi-design.md)) | A2 | fertig |
| REND-02 | Vulkan-Backend | Vulkan 1.4, Dynamic Rendering, Synchronization2, volk, VMA, Swapchain | A2 | fertig |
| REND-03 | Mindestanforderung prüfen | GPU beim Start gegen [ADR 0014](adr/0014-modern-gpu-baseline.md) prüfen, Fehlendes protokollieren, alles einschalten | A2 | fertig |
| REND-04 | Shader mit Slang | Beim Bauen zu SPIR-V kompiliert und eingebettet; Reflection folgt in R1 | A2 | fertig |
| REND-05 | Render-Graph | Passes und Ressourcen deklarieren, Barrieren automatisch | R1 | geplant |
| REND-06 | Bindless | Ressourcen über Indizes statt Descriptor-Sets pro Draw | R1 | geplant |
| REND-07 | PBR-Materialien | Physikalisch basiertes Shading, Material-Instanzen | R1 | geplant |
| REND-08 | Kamera | Perspektive, Steuerung, kamerarelatives Rendering | R1 | geplant |
| REND-09 | Licht und Schatten | Sonne mit Cascaded Shadow Maps, Punkt- und Spotlichter | R1 | geplant |
| REND-10 | GPU-Culling | Frustum- und Occlusion-Culling auf der GPU | R1 | Idee |
| REND-11 | Post-Processing | Tone Mapping, Bloom, Farbkorrektur | R1 | geplant |
| REND-12 | Globale Beleuchtung | Dynamische GI für veränderbare Welten (z. B. Probes oder Radiance Cascades) | R3 | Idee |
| REND-13 | Mesh Shader | GPU-getriebene Geometrie mit Meshlets und Culling auf der GPU; einziger Geometriepfad | R1 | geplant |
| REND-14 | Raytracing | Schatten, AO und GI über Ray Queries; Nutzung pro Effekt als Qualitätseinstellung | R2 | geplant |
| REND-15 | Debug-Zeichnen | Linien, Boxen, Text in der Welt | R1 | geplant |

## Welt und ECS

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| WORLD-01 | ECS | flecs hinter `World` und `Entity` | A4 | geplant |
| WORLD-02 | Entity-API | Bequeme Schicht im Stil von GameObjects, auch für C# | A4 | geplant |
| WORLD-03 | Hierarchie | Eltern-Kind-Beziehungen, Transform-Propagation | A4 | geplant |
| WORLD-04 | Prefabs | Vorlagen für Entities, Instanzen mit Überschreibungen | R2 | geplant |
| WORLD-05 | Szenen speichern und laden | Über Reflection und Serialisierung | A4 | geplant |
| WORLD-06 | 64-Bit-Weltpositionen | Große Welten ohne Zittern | R1 | geplant |
| WORLD-07 | Welt-Partitionierung | Räumliche Aufteilung für Sichtbarkeit und Streaming | R3 | geplant |
| WORLD-08 | Streaming | Welt-Teile nach Kameraposition laden und entladen | R3 | geplant |

## Voxel-Terrain

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| VOX-01 | Dichtefeld | Dichte und Material pro Voxel, in Chunks | D1 / R3 | geplant |
| VOX-02 | Meshing | Surface Nets oder Dual Contouring | D1 / R3 | geplant |
| VOX-03 | Bearbeiten zur Laufzeit | Abbauen, Auftragen, Glätten, ohne Ruckler | D1 / R3 | geplant |
| VOX-04 | LOD | Gröbere Chunks in der Ferne ohne Risse | R3 | geplant |
| VOX-05 | Prozedurale Welt | Grundwelt aus Rauschen und Regeln | R3 | geplant |
| VOX-06 | Deltas | Nur Änderungen speichern und übertragen | R3 / R5 | geplant |
| VOX-07 | Materialien | Mehrere Materialien pro Oberfläche mit Übergängen (Triplanar) | R3 | geplant |
| VOX-08 | Kollision | Physik-Formen aus den Voxeln | R4 | geplant |
| VOX-09 | Höhlen und Überhänge | Echte 3D-Strukturen statt Höhenkarte | R3 | geplant |
| VOX-10 | Statik | Einstürzende Strukturen ohne Halt | später | Idee |

## Natur und Atmosphäre

Aus der Spielidee abgeleitet: faszinierende Natur mit Horror-Elementen.

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| NAT-01 | Vegetation | Viele Pflanzen per Instancing, auf dem Voxel-Terrain verteilt | R3 | geplant |
| NAT-02 | Wind | Bewegung von Gras und Bäumen im Shader | R3 | Idee |
| NAT-03 | Tag-Nacht-Wechsel | Sonne, Mond, Himmel, Lichtstimmung über Zeit | R1 / R3 | geplant |
| NAT-04 | Himmel und Atmosphäre | Physikalisch basierte Atmosphäre | R1 | geplant |
| NAT-05 | Volumetrischer Nebel | Nebel mit Lichtstreuung, wichtig für Horror-Stimmung | R3 | Idee |
| NAT-06 | Wetter | Regen, Sturm, Wolken | später | Idee |
| NAT-07 | Wasser | Flüsse, Seen | später | Idee |

## Physik

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| PHYS-01 | Physik-Welt | Jolt hinter `PhysicsWorld` | R4 | geplant |
| PHYS-02 | Starre Körper | Kollision und Dynamik | R4 | geplant |
| PHYS-03 | Character Controller | Bewegung von Spielfiguren | R4 | geplant |
| PHYS-04 | Raycasts und Abfragen | Für Gameplay, Bauen, KI | R4 | geplant |

## Animation

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| ANIM-01 | Skelett-Animation | Skinning auf der GPU | R4 | geplant |
| ANIM-02 | Blending | Übergänge, Blend Trees, Zustandsautomaten | R4 | geplant |
| ANIM-03 | Inverse Kinematik | Füße auf unebenem Terrain | später | Idee |

## Audio

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| AUD-01 | Audio-Gerät | miniaudio hinter `AudioDevice` | R4 | geplant |
| AUD-02 | 3D-Klang | Positionierung, Abschwächung | R4 | geplant |
| AUD-03 | Hall und Verdeckung | Räume und Wände beeinflussen den Klang, wichtig für Horror | später | Idee |

## Scripting

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| SCR-01 | .NET-Hosting | Eingebettetes .NET über hostfxr | D2 / R4 | geplant |
| SCR-02 | Bindings | Aus der Reflection erzeugte C#-API | R4 | geplant |
| SCR-03 | Skript-Komponenten | Gameplay-Logik als C#-Komponenten | R4 | geplant |
| SCR-04 | Hot Reload | Skripte zur Laufzeit neu laden | R4 | geplant |

## Netzwerk

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| NET-01 | Transport | GameNetworkingSockets hinter `Transport` | D3 / R5 | geplant |
| NET-02 | Dedizierter Server | Headless-Build ohne Grafik | D3 / R5 | geplant |
| NET-03 | Listen-Server | Ein Spieler hostet | R5 | geplant |
| NET-04 | Replikation | Geänderte Komponenten an Clients senden | D3 / R5 | geplant |
| NET-05 | Vorhersage | Client-seitige Vorhersage und Korrektur | R5 | geplant |
| NET-06 | Voxel-Synchronisation | Terrain-Änderungen effizient verteilen | R5 | geplant |

## Assets

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| ASSET-01 | glTF-Import | Meshes und Materialien über fastgltf | R1 | geplant |
| ASSET-02 | Texturen | KTX2 mit Basis Universal, Quellbilder über stb_image | R1 | geplant |
| ASSET-03 | Asset-Pipeline | Quelldaten in Laufzeitformate umwandeln („Cooking“) | R2 | geplant |
| ASSET-04 | Ressourcen-Manager | Laden, Referenzen, Entladen, asynchron | R2 | geplant |
| ASSET-05 | Asset-IDs | Stabile IDs statt Pfade | R2 | geplant |

## Editor

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| EDIT-01 | Editor-Gerüst | Eigene Anwendung mit Dear ImGui und Docking | R2 | geplant |
| EDIT-02 | Viewport | Szene im Editor rendern, Kamera steuern | R2 | geplant |
| EDIT-03 | Szenenbaum | Entities und Hierarchie | R2 | geplant |
| EDIT-04 | Inspector | Komponenten über Reflection bearbeiten | R2 | geplant |
| EDIT-05 | Gizmos | Verschieben, Drehen, Skalieren | R2 | geplant |
| EDIT-06 | Spielen im Editor | Spiel im Editor starten und stoppen | R4 | geplant |
| EDIT-07 | Undo und Redo | Alle Editor-Aktionen umkehrbar | R2 | geplant |
| EDIT-08 | Voxel-Werkzeuge | Terrain im Editor formen und bemalen | R3 | geplant |
| EDIT-09 | Asset-Browser | Assets finden, importieren, Vorschau | R2 | geplant |
| EDIT-10 | Log-Konsole | Log-Sink, der im Editor anzeigt | R2 | geplant |

## Spiel-UI

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| UI-01 | HUD und Menüs | Oberfläche im Spiel, getrennt vom Editor (z. B. RmlUi) | R4 | Idee |
| UI-02 | Text | Schriften, Unicode, Umlaute | R4 | geplant |

## KI und Navigation

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| AI-01 | Navigation | Navigationsnetze auf veränderbarem Terrain | später | Idee |
| AI-02 | Verhalten | Verhaltensbäume oder Utility-KI | später | Idee |

## Werkzeuge und Build

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| TOOL-01 | CMake mit Modulen | Presets für Linux und Windows | A1 | fertig |
| TOOL-02 | CI | Bauen und testen auf Linux und Windows | A1 | fertig |
| TOOL-03 | Tests | Catch2 | A1 | fertig |
| TOOL-04 | Doku im Repository | Docs as Code, ADRs, Mermaid | K1 | fertig |
| TOOL-05 | Doku-Prüfung in CI | Kaputte Links und Formatierung prüfen | A2 | Idee |
| TOOL-06 | Formatierung in CI | clang-format prüfen | A2 | geplant |
| TOOL-07 | Pakete | Installierbare Builds für Linux und Windows | Übergabe | geplant |
| TOOL-08 | GPU-Tests in CI | Vulkan-Tests und Sandbox mit Fenster über lavapipe und Xvfb | A2 | fertig |

## Ideen-Parkplatz

Ungeordnete Ideen für Brainstorming. Beim nächsten Review entscheiden wir, ob sie in eine Komponente
wandern, und mit welchem Status.

- Perspektivische Rätsel wie in Superliminal: Objekte ändern ihre Größe je nach Blickwinkel
- Unzuverlässige Wahrnehmung für Horror: Dinge verändern sich, wenn man nicht hinsieht
- Dunkelheit als Spielmechanik: Lichtquellen sind Ressourcen
- Prozedurale Vegetation, die auf Jahreszeiten reagiert
- Replays durch deterministische Simulation
