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
| [R1](R1-rendering-grundlagen.md) | Konstruktion | Reifegrad Rendering-Grundlagen (R1.1 bis R1.4) | in Arbeit |
| [R1.1](R1.1-mathematik-und-transforms.md) | Konstruktion | Mathematik und Transforms | abgeschlossen |
| [R1.2](R1.2-render-graph-bindless-gpu-speicher.md) | Konstruktion | Render-Graph, Bindless, GPU-Speicher | abgeschlossen |
| [R1.3](R1.3-meshes-materialien-kamera.md) | Konstruktion | Meshes, Materialien, Kamera | in Arbeit |

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
