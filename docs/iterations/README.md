# Iterationen

Jede Iteration bekommt ein Dokument mit Plan und Review (siehe [Prozess](../process.md)). Der Plan
entsteht vor dem ersten PR, das Review nach dem letzten Merge.

| Iteration | Phase | Ziel | Status |
| --- | --- | --- | --- |
| [A1](A1-projektgeruest.md) | Ausarbeitung | Projektgerüst mit CMake, Modulen, Core und CI | abgeschlossen |
| [A2](A2-fenster-und-dreieck.md) | Ausarbeitung | Fenster und Vulkan-Dreieck | geplant |

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
```
