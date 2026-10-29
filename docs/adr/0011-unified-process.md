# 0011: Vereinfachter Unified Process

- **Status:** angenommen
- **Datum:** 2026-10-29

## Kontext

Das Projekt ist ein Lernprojekt mit wechselnden Anforderungen (Moving Target), großen technischen
Risiken und zwei Beteiligten. Wir brauchen Struktur, aber keinen schweren Prozess.

## Entscheidung

Wir nutzen einen vereinfachten Unified Process: Phasen mit Meilensteinen, kurze Iterationen mit Plan
und Review, risikogetriebene Durchstiche in der Ausarbeitung. Details in [process.md](../process.md).
Das bisherige Reifegradmodell lebt als Reihenfolge der Konstruktion weiter.

## Konsequenzen

- Die größten Risiken (Voxel-Meshing, C#-Hosting, Netzwerk) werden früher geprüft als im
  ursprünglichen Reifegradmodell.
- Jede Iteration erzeugt ein kurzes Dokument mit Plan und Review.

## Erwogene Alternativen

- **Scrum:** Rollen und feste Sprints passen nicht zu einem Hobby-Rhythmus.
- **Kanban ohne Phasen:** zu wenig Struktur für Architekturentscheidungen.
- **Klassischer RUP:** zu viele Artefakte und Rollen.
