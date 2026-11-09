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
| CORE-07 | Profiling | Tracy-Zonen für CPU (A3) und GPU (pro Pass des Render-Graphs über Zeitstempel, `render::GpuProfiler`, R1.3), Thread-Namen, Plots | A3 / R1.3 | fertig |
| CORE-08 | Reflection | Typinformationen für Editor, Speichern, Netzwerk und C#: `ngine.reflect` mit Builder-API ([ADR 0018](adr/0018-own-reflection.md)); Enums, Arrays und Entity-Verweise folgen bei Bedarf | A4 | fertig |
| CORE-09 | Serialisierung | JSON mit eigenem Parser und Binärformat, beides über Reflection ([ADR 0020](adr/0020-scene-format-json.md)) | A4 | fertig |
| CORE-10 | Mathematik | Eigene Vektoren, Matrizen, Quaternionen und Projektionen in `ngine.math`, gegen GLM getestet; `Affine` mit 64-Bit-Verschiebung ([ADR 0025](adr/0025-own-math-mixed-precision.md)) | R1.1 | fertig |
| CORE-11 | Ereignisse | Lose gekoppelte Nachrichten zwischen Systemen (`events::EventBus` in `ngine.core`, sofort oder gesammelt pro Frame) | R2.2 | fertig |
| CORE-12 | Log-Datei | Sink, der in eine Datei schreibt (`log::FileSink`, der Editor schreibt in den Benutzerordner) | R2.1 | fertig |

## Plattform

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| PLAT-01 | Fenster | Fenster über SDL3, Wayland und X11 unter Linux | A2 | fertig |
| PLAT-02 | Eingabe | Tastatur, Maus, Gamepad, Abbildung auf Aktionen; bisher Tasten nach Scancode (Buchstaben, Ziffern, F1 bis F12, Navigation, Umschalttasten) mit Loslassen, Texteingabe, Fokus, Maus (Bewegung, relativer Modus, Tasten, Rad, Zeigerform), Zwischenablage | A2 / D1 / R1.3 / R2.1 | in Arbeit |
| PLAT-03 | Headless-Start | Engine läuft ohne Fenster und GPU (Server, Tests); Build ohne SDL3 mit `NGINE_WITH_WINDOW=OFF` | A1 / A2 | fertig |
| PLAT-04 | Dateisystem | Dateien lesen und atomar schreiben, Benutzerordner, UTF-8-Pfade (`ngine.filesystem`, auch ohne Fenster) | A4 | fertig |
| PLAT-05 | Virtuelles Dateisystem | Archive, Mounts, einheitliche Pfade | R2 | Idee |
| PLAT-06 | Hot Reload von Dateien | Änderungen an Shadern und Assets zur Laufzeit erkennen: `FileWatcher` über Polling, Shader über Slang zur Laufzeit (`ngine.shader_compiler`), Modelle und Texturen über den Ressourcen-Manager ([ADR 0034](adr/0034-hot-reload-polling-slang-runtime.md)) | R2.4 | fertig |

## Rendering

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| REND-01 | RHI | Schmale Schnittstelle über Vulkan; später D3D12 und Metal ([ADR 0013](adr/0013-rhi-design.md)); Puffer, Tiefe, Push Constants und indiziertes Zeichnen seit D1 | A2 / D1 | fertig |
| REND-16 | Vertex Pulling | Geometrie über Buffer Device Address statt Vertex-Attribute ([ADR 0022](adr/0022-vertex-pulling-bda.md)); statische Geometrie im GPU-Speicher über `upload()` (R1.2) | D1 / R1.2 | fertig |
| REND-02 | Vulkan-Backend | Vulkan 1.3 (1.4, wo vorhanden), Dynamic Rendering, Synchronization2, volk, VMA, Swapchain | A2 | fertig |
| REND-03 | Mindestanforderung und Feature-Stufen | GPU beim Start gegen [ADR 0017](adr/0017-platform-baseline-feature-tiers.md) prüfen, Fehlendes protokollieren; Mesh Shader und Raytracing erkennen, einschalten und in `Capabilities::features` melden, abschaltbar für Tests | A2 / A3 | fertig |
| REND-04 | Shader mit Slang | Beim Bauen zu SPIR-V kompiliert und eingebettet; Reflection folgt in R1 | A2 | fertig |
| REND-05 | Render-Graph | Passes und Ressourcen deklarieren, Barrieren, Verwerfen und transiente Texturen automatisch (`ngine.render`, [ADR 0026](adr/0026-render-graph-bindless.md)) | R1.2 | fertig |
| REND-06 | Bindless | Texturen und Sampler über Indizes in einem Descriptor-Satz statt Sätzen pro Draw ([ADR 0026](adr/0026-render-graph-bindless.md)) | R1.2 | fertig |
| REND-07 | PBR-Materialien | glTF Metallic-Roughness mit GGX, Smith und Schlick; Materialien als Feld im GPU-Speicher mit bindlosen Texturen (`MaterialStore`, [ADR 0027](adr/0027-gltf-assets-material-store.md)); Alpha-Maske und unsortiertes Überblenden | R1.3 | fertig |
| REND-08 | Kamera | Komponente `Camera` mit umgekehrter Tiefe und unendlicher Ferne, kamerarelativ über `WorldTransform`; freie Steuerung in der Sandbox | R1.3 | fertig |
| REND-09 | Licht und Schatten | Lichter als Komponenten (`DirectionalLight`, `PointLight`, `SpotLight`); Sonne mit vier Cascaded Shadow Maps, eingerastet gegen Flimmern; Punkt- und Spotlichter ohne Schatten aus einer Liste im GPU-Speicher ([ADR 0028](adr/0028-lights-cascaded-shadows.md)) | R1.4 | fertig |
| REND-10 | GPU-Culling | Frustum- und Occlusion-Culling auf der GPU | R3 | Idee |
| REND-11 | Post-Processing | Tone Mapping (Khronos PBR Neutral, Belichtung aus der Kamera, R1.3), Bloom, Farbkorrektur | R1.3 / R3 | in Arbeit |
| REND-12 | Globale Beleuchtung | Dynamische GI für veränderbare Welten (z. B. Probes oder Radiance Cascades) | R3 | Idee |
| REND-13 | Mesh Shader | GPU-getriebene Geometrie mit Meshlets und Culling auf der GPU, wo die Stufe vorhanden ist; sonst Vertex-Pipeline mit Compute-Culling | R3 | geplant |
| REND-14 | Raytracing | Schatten, AO und GI über Ray Queries, wo die Stufe vorhanden ist; sonst Shadow Maps und Screen-Space-Verfahren; aus R2 verschoben, Iteration legt spätestens das Review von R3 fest | nach R3 | geplant |
| REND-15 | Debug-Zeichnen | Linien, Boxen, Kugeln und Achsenkreuze in der Welt (`Renderer::debug()`, R1.4); Text in der Welt über `DebugDraw::text()`, im Editor über die Zeichenliste der UI gezeichnet (R2.3) | R1.4 / R2.3 | fertig |

## Welt und ECS

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| WORLD-01 | ECS | flecs hinter `World`, `Entity` und `Query`; Systeme über `Schedule` auf dem Job-System ([ADR 0019](adr/0019-systems-on-job-system.md)) | A4 | fertig |
| WORLD-02 | Entity-API | Bequeme Schicht im Stil von GameObjects (`entity.set(...)`, `get<T>()`, `name()`); die C#-Seite folgt mit D2 / R4 | A4 | in Arbeit |
| WORLD-03 | Hierarchie | Eltern-Kind-Beziehungen (A4); `Transform` und `WorldTransform`, Propagation Eltern vor Kindern mit `update_transforms()` (R1.1), nur für geänderte Teilbäume (R2.3) | A4 / R1.1 / R2.3 | fertig |
| WORLD-04 | Prefabs | Vorlagen als `.prefab`-Datei, Instanzen in der Welt voll ausgebaut, in Szenen als Kennung mit Überschreibungen auf Feldebene; geänderte Vorlagen ziehen alle Instanzen nach (Hot Reload), im Editor anlegen, übernehmen, aufbrechen und aus dem Asset-Browser ziehen ([ADR 0037](adr/0037-prefabs-field-overrides.md)) | R2.5 | fertig |
| WORLD-05 | Szenen speichern und laden | JSON über Reflection, Hierarchie bleibt erhalten (A4); binär als `.nscene` mit Typtabelle (R2.5) | A4 / R2.5 | fertig |
| WORLD-06 | 64-Bit-Weltpositionen | Große Welten ohne Zittern: Positionen in 64 Bit, gezeichnet relativ zur Kamera ([ADR 0012](adr/0012-64bit-world-coordinates.md)); `--far` zeigt es | R1.1 | fertig |
| WORLD-07 | Welt-Partitionierung | Räumliche Aufteilung für Sichtbarkeit und Streaming; R3.3: Baum von Chunks in Stufen, ausgeglichen, um mehrere Beobachter ([ADR 0041](adr/0041-lod-octree-surface-nets-streaming.md)) | R3 | in Arbeit |
| WORLD-08 | Streaming | Welt-Teile nach Kameraposition laden und entladen; R3.3: Terrain lädt um `TerrainObserver` im Job-System mit Budget, Übernahme in einem Schritt, Speicher begrenzt; Budget für Uploads folgt (TD-39) | R3 | in Arbeit |
| WORLD-09 | Befehlspuffer | Strukturänderungen aus Systemen heraus sammeln und nach dem Schedule ausführen | R4 | geplant |
| WORLD-10 | Phasen für Systeme | Systeme in Phasen (Eingabe, Simulation, Rendering) und mit expliziter Reihenfolge | R4 | geplant |

## Voxel-Terrain

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| VOX-01 | Dichtefeld | Dichte (8 Bit, Abstand) und Material pro Sample ([ADR 0021](adr/0021-surface-nets-8bit-density.md)); D1: ein Gitter; R3.1: Chunks mit 32³ Samples zu 0,5 m, leere und volle Chunks als ein Wert ([ADR 0038](adr/0038-terrain-chunks-shared-geometry.md)) | D1 / R3 | in Arbeit |
| VOX-02 | Meshing | Surface Nets ([ADR 0021](adr/0021-surface-nets-8bit-density.md)); D1: ein Chunk; R3.1: nahtlose Chunk-Grenzen über eine Randschicht der Nachbarn, Meshing im Job-System; R3.3: `LodMesher` über Stufen des LOD-Baums | D1 / R3 | in Arbeit |
| VOX-03 | Bearbeiten zur Laufzeit | Abbauen, Auftragen, Glätten, ohne Ruckler; D1: Kugel-Pinsel und Strahltest, Neu-Meshing im selben Frame; R3.1: Pinsel und Strahltest über Chunk-Grenzen, nur betroffene Chunks neu gemesht | D1 / R3 | in Arbeit |
| VOX-04 | LOD | Gröbere Chunks in der Ferne ohne Risse; R3.3: Surface Nets über Stufengrenzen mit Nahtpolygonen, Test ohne offene Kanten ([ADR 0041](adr/0041-lod-octree-surface-nets-streaming.md)) | R3 | in Arbeit |
| VOX-05 | Prozedurale Welt | Grundwelt aus Rauschen und Regeln; R3.2: eigenes deterministisches Rauschen und Generator mit Hügeln, Felswänden und Schichten, bitgleich auf Linux und Windows ([ADR 0039](adr/0039-deterministic-noise-rule-generator.md)); R3.3: grobe Stufen und endloses Terrain mit Streaming | R3 | in Arbeit |
| VOX-06 | Deltas | Nur Änderungen speichern und übertragen | R3 / R5 | geplant |
| VOX-07 | Materialien | Mehrere Materialien pro Oberfläche mit Übergängen (Triplanar); R3.2: Gewichte je Eckpunkt mit Palette pro Chunk, Tabelle mit Texturen, triplanar im Shader ([ADR 0040](adr/0040-terrain-materials-weights-triplanar.md)); Materialtabelle als Asset folgt (TD-37) | R3 | in Arbeit |
| VOX-08 | Kollision | Physik-Formen aus den Voxeln | R4 | geplant |
| VOX-09 | Höhlen und Überhänge | Echte 3D-Strukturen statt Höhenkarte; R3.2: Überhänge an steilen Stellen, Tunnel und Kavernen aus 3D-Rauschen | R3 | in Arbeit |
| VOX-10 | Statik | Einstürzende Strukturen ohne Halt | später | Idee |

## Natur und Atmosphäre

Aus der Spielidee abgeleitet: faszinierende Natur mit Horror-Elementen.

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| NAT-01 | Vegetation | Viele Pflanzen per Instancing, auf dem Voxel-Terrain verteilt | nach R3 | geplant |
| NAT-02 | Wind | Bewegung von Gras und Bäumen im Shader | nach R3 | Idee |
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
| SCR-01 | .NET-Hosting | Eingebettetes .NET 10 über hostfxr, Aufrufe in beide Richtungen über Funktionszeiger ([ADR 0023](adr/0023-dotnet-hosting-hostfxr.md)) | D2 / R4 | in Arbeit |
| SCR-02 | Bindings | Aus der Reflection erzeugte C#-API | R4 | geplant |
| SCR-03 | Skript-Komponenten | Gameplay-Logik als C#-Komponenten | R4 | geplant |
| SCR-04 | Hot Reload | Skripte zur Laufzeit neu laden; D2: Entladen und Neuladen über einen entladbaren Kontext funktioniert, Dateiüberwachung fehlt | D2 / R4 | in Arbeit |

## Netzwerk

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| NET-01 | Transport | GameNetworkingSockets hinter `Transport` ([ADR 0024](adr/0024-networking-gns-snapshots.md)) | D3 / R5 | in Arbeit |
| NET-02 | Dedizierter Server | Headless-Build ohne Grafik (`ngine_sandbox --server`) | D3 / R5 | in Arbeit |
| NET-03 | Listen-Server | Ein Spieler hostet | R5 | geplant |
| NET-04 | Replikation | Geänderte Komponenten an Clients senden; in D3 volle Schnappschüsse über die Reflection | D3 / R5 | in Arbeit |
| NET-05 | Vorhersage | Client-seitige Vorhersage und Korrektur | R5 | geplant |
| NET-06 | Voxel-Synchronisation | Terrain-Änderungen effizient verteilen | R5 | geplant |

## Assets

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| ASSET-01 | glTF-Import | `.gltf` und `.glb` über fastgltf in CPU-Daten (`ngine.asset`): Meshes, Materialien, Bilder, Hierarchie; Modelle als Entities ([ADR 0027](adr/0027-gltf-assets-material-store.md)) | R1.3 | fertig |
| ASSET-02 | Texturen | Quellbilder über stb_image mit Mip-Stufen (R1.3, ab R2.4 auf der CPU); gekocht als KTX2 mit Basis Universal (UASTC), geladen als BC7 oder RGBA8 je nach GPU (R2.5, [ADR 0036](adr/0036-cooked-assets-ktx2-cache.md)) | R1.3 / R2.5 | fertig |
| ASSET-03 | Asset-Pipeline | glTF zu gekochten Modellen mit MikkTSpace-Tangenten und KTX2-Texturen in einem Cache, neu gekocht nur bei geänderten Quellen; im Editor beim Laden, mit „Alles kochen“ oder mit `ngine_cook` ([ADR 0036](adr/0036-cooked-assets-ktx2-cache.md)) | R2.5 | fertig |
| ASSET-04 | Ressourcen-Manager | Handles mit Referenzzählung, Laden im Hintergrund über das Job-System, Entladen ungenutzter Ressourcen, Neuladen samt Abhängigkeiten; Uploads über eine eigene Transfer-Queue ([ADR 0033](adr/0033-asset-ids-meta-resource-manager.md), [ADR 0035](adr/0035-transfer-queue-concurrent-sharing.md)) | R2.4 | fertig |
| ASSET-05 | Asset-IDs | Stabile 64-Bit-Kennungen in `.meta`-Dateien neben den Quellen, `AssetRef` für Teile eines Assets; `MeshRenderer` verweist darüber, Szenen mit Modellen sind vollständig speicherbar ([ADR 0033](adr/0033-asset-ids-meta-resource-manager.md)) | R2.4 | fertig |

## Editor

| ID | Feature | Beschreibung | Wann | Status |
| --- | --- | --- | --- | --- |
| EDIT-01 | Editor-Gerüst | Eigene Anwendung mit Dear ImGui und Docking (`apps/editor/`, `ngine.editor_ui`, [ADR 0029](adr/0029-editor-imgui-over-rhi.md)) | R2.1 | fertig |
| EDIT-02 | Viewport | Szene im Editor rendern, Kamera steuern (Textur, die dem Fenster folgt; freie Kamera wie in der Sandbox) | R2.1 | fertig |
| EDIT-03 | Szenenbaum | Entities und Hierarchie; Auswahl auch per Klick im Viewport (ID-Puffer), Umbenennen, Umhängen per Ziehen | R2.2 | fertig |
| EDIT-04 | Inspector | Komponenten über Reflection bearbeiten; Felder allein aus der Reflection mit Enums, Bereichen und Hinweisen (Farbe, Rotation, Winkel), `ngine.edit` | R2.2 | fertig |
| EDIT-05 | Gizmos | Verschieben, Drehen, Skalieren, lokal und global, mit Raster und Undo; Treffertests über Strahlen in `ngine.edit` ([ADR 0031](adr/0031-gizmos-own-math-ui-draw-list.md)) | R2.3 | fertig |
| EDIT-06 | Spielen im Editor | Spiel im Editor starten, pausieren und stoppen; die Welt kommt bitgenau zurück ([ADR 0032](adr/0032-play-in-editor-binary-snapshot.md)). C#-Skripte im Spielmodus folgen mit R4 | R2.3 / R4 | in Arbeit |
| EDIT-07 | Undo und Redo | Alle Editor-Aktionen umkehrbar; Befehle mit reflektierten Werten ([ADR 0030](adr/0030-undo-commands-reflected-values.md)) | R2.2 | fertig |
| EDIT-08 | Voxel-Werkzeuge | Terrain im Editor formen und bemalen | R3 | geplant |
| EDIT-09 | Asset-Browser | Assets finden (Liste mit Filter) und Modelle ins Viewport ziehen (Undo-Schritt); seit R2.5 auch Prefabs ziehen und alle Modelle kochen; Vorschaubilder und Import fremder Formate folgen später | R2.4 / R2.5 | in Arbeit |
| EDIT-10 | Log-Konsole | Log-Sink, der im Editor anzeigt (`log::RingSink`, `editor_ui::LogConsole` mit Filter nach Stufe und Text) | R2.1 | fertig |

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
| TOOL-08 | GPU-Tests in CI | Vulkan-Tests und Sandbox mit Fenster über lavapipe und Xvfb, auch ohne optionale Stufen (`--baseline`) | A2 | fertig |

## Ideen-Parkplatz

Ungeordnete Ideen für Brainstorming. Beim nächsten Review entscheiden wir, ob sie in eine Komponente
wandern, und mit welchem Status.

- Perspektivische Rätsel wie in Superliminal: Objekte ändern ihre Größe je nach Blickwinkel
- Unzuverlässige Wahrnehmung für Horror: Dinge verändern sich, wenn man nicht hinsieht
- Dunkelheit als Spielmechanik: Lichtquellen sind Ressourcen
- Prozedurale Vegetation, die auf Jahreszeiten reagiert
- Replays durch deterministische Simulation
