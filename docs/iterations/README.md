# Iterationen

Jede Iteration bekommt ein Dokument mit Plan und Review (siehe [Prozess](../process.md)). Der Plan
entsteht vor dem ersten PR, das Review nach dem letzten Merge.

| Iteration | Phase | Ziel | Status |
| --- | --- | --- | --- |
| [A1](A1-projektgeruest.md) | Ausarbeitung | Projektgerüst mit CMake, Modulen, Core und CI | abgeschlossen |
| [A2](A2-fenster-und-dreieck.md) | Ausarbeitung | Fenster und Vulkan-Dreieck | abgeschlossen |
| [A3](A3-jobs-und-speicher.md) | Ausarbeitung | Job-System und Speicher | abgeschlossen |
| [A4](A4-ecs-und-reflection.md) | Ausarbeitung | ECS und Reflection | abgeschlossen |
| [D1](D1-voxel-meshing.md) | Ausarbeitung | Durchstich Voxel-Meshing | abgeschlossen |
| [D2](D2-csharp-hosting.md) | Ausarbeitung | Durchstich C#-Hosting | abgeschlossen |
| [D3](D3-netzwerk.md) | Ausarbeitung | Durchstich Netzwerk | abgeschlossen |
| [R1](R1-rendering-grundlagen.md) | Konstruktion | Reifegrad Rendering-Grundlagen (R1.1 bis R1.4) | abgeschlossen |
| [R1.1](R1.1-mathematik-und-transforms.md) | Konstruktion | Mathematik und Transforms | abgeschlossen |
| [R1.2](R1.2-render-graph-bindless-gpu-speicher.md) | Konstruktion | Render-Graph, Bindless, GPU-Speicher | abgeschlossen |
| [R1.3](R1.3-meshes-materialien-kamera.md) | Konstruktion | Meshes, Materialien, Kamera | abgeschlossen |
| [R1.4](R1.4-licht-und-schatten.md) | Konstruktion | Licht und Schatten | abgeschlossen |
| [R2](R2-welt-und-editor.md) | Konstruktion | Reifegrad Welt und Editor (R2.1 bis R2.5) | abgeschlossen |
| [R2.1](R2.1-editor-geruest-und-viewport.md) | Konstruktion | Editor-Gerüst und Viewport | abgeschlossen |
| [R2.2](R2.2-szenenbaum-inspector-undo.md) | Konstruktion | Szenenbaum, Inspector, Undo | abgeschlossen |
| [R2.3](R2.3-gizmos-und-spielen.md) | Konstruktion | Gizmos und Spielen im Editor | abgeschlossen |
| [R2.4](R2.4-asset-ids-ressourcen-hot-reload.md) | Konstruktion | Asset-IDs, Ressourcen und Hot Reload | abgeschlossen |
| [R2.5](R2.5-kochen-ktx2-prefabs.md) | Konstruktion | Kochen, KTX2 und Prefabs | abgeschlossen |
| [R3](R3-voxel-terrain.md) | Konstruktion | Reifegrad Voxel-Terrain (R3.1 bis R3.5) | abgeschlossen |
| [R3.1](R3.1-chunks-und-meshing.md) | Konstruktion | Chunks und Meshing in Jobs | abgeschlossen |
| [R3.2](R3.2-prozedurale-welt-und-materialien.md) | Konstruktion | Prozedurale Welt und Materialien | abgeschlossen |
| [R3.3](R3.3-streaming-und-lod.md) | Konstruktion | Streaming und LOD | abgeschlossen |
| [R3.4](R3.4-deltas-und-voxel-werkzeuge.md) | Konstruktion | Deltas und Voxel-Werkzeuge | abgeschlossen |
| [R3.5](R3.5-gpu-getriebenes-zeichnen.md) | Konstruktion | GPU-getriebenes Zeichnen | abgeschlossen |
| [R4](R4-gameplay-systeme.md) | Konstruktion | Reifegrad Gameplay-Systeme (R4.1 bis R4.6) | geplant |

## Vorlage

```markdown
# <ID>: <Ziel>

- **Phase:** …
- **Status:** geplant | in Arbeit | abgeschlossen
- **Pull Requests:** …

## Plan

**Ziel:** ein Satz.

**Features:** IDs aus dem Feature-Katalog.

**Risiken:** was schiefgehen kann.

**Abnahme:** woran wir erkennen, dass die Iteration fertig ist.

## Review

**Ergebnis:** was erreicht wurde.

**Erkenntnisse:** was wir gelernt haben.

**Anpassungen:** was sich an Vision, Roadmap, Katalog oder ADRs geändert hat.

**Architektur-Review:** Ergebnis der Fragen aus dem Prozess, neue Einträge im Schuldenregister.
```
