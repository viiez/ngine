# 0038: Terrain aus Chunks mit Randschicht, Terrain als Komponente, gemeinsame Geometriepuffer

- **Status:** angenommen
- **Datum:** 2026-11-08

## Kontext

Der Durchstich D1 hatte ein einziges Gitter mit 33³ Samples, dessen Netz am Rand offen blieb. R3
braucht eine große Welt aus vielen Chunks, die nahtlos aneinanderpassen, in Jobs gemesht und
einzeln neu gemesht werden, wenn ein Pinsel sie trifft. Später kommen Streaming, LOD und Deltas
dazu (R3.3, R3.4), daher müssen Speicher pro Chunk klein und die Grenzen einfach bleiben.

Im Renderer hatte jedes Netz eigene Eckpunkt- und Indexpuffer (TD-23). Mit Hunderten Chunks, die
sich laufend ändern, werden das zu viele kleine Allokationen, und GPU-getriebenes Zeichnen in R3.5
braucht ohnehin wenige große Puffer.

## Entscheidung

- **Chunks:** Ein Chunk besitzt 32³ Samples zu 0,5 m (16 m). Chunk-Koordinaten sind `i32` mit
  einer Stufe für LOD (heute immer 0), Samples werden mit `i64` adressiert, die Lage in der Welt ist
  64 Bit ([ADR 0012](0012-64bit-world-coordinates.md)). Leere und volle Chunks halten nur einen
  Wert und werden nach jedem Füllen oder Pinselstrich wieder geschrumpft.
- **Nahtlos über eine Randschicht:** Zum Meshen kopiert `gather()` den Chunk mit je 2 Samples der
  Nachbarn davor und danach (36³). Eckpunkte entstehen in allen Zellen, die eigene Vierecke
  berühren; Vierecke nur für Kanten, die an eigenen Samples beginnen. Jedes Viereck gehört damit
  genau einem Chunk, und ein Eckpunkt an der Grenze wird in beiden Chunks aus denselben Samples
  berechnet. Die zweite Schicht braucht der Mesher für Normalen über Gradienten.
- **Vormerkung:** Der `ChunkStore` merkt sich jeden Chunk, dessen Randschicht ein geändertes
  Sample enthält, auch Chunks, die es nicht gibt. Ihre Netze schließen das Terrain am Rand.
- **Terrain als Komponente:** `world::Terrain` beschreibt Größe und Form und wird in der Szene
  gespeichert; Chunks und Netze liegen in der transienten Komponente `TerrainState` hinter einem
  `shared_ptr`. `update_terrains()` erzeugt bei geänderter Beschreibung alles neu und meshet sonst
  nur vorgemerkte Chunks, beides im Job-System. `ngine.world` hängt dafür an `ngine.voxel`.
- **Gemeinsame Geometriepuffer:** Der Renderer hält Blöcke zu 32 MiB; Bereiche vergibt ein
  `RangeAllocator` (first fit, freie Nachbarn verschmelzen). Freigaben greifen erst nach
  `frames_in_flight + 2` Frames, damit kein Frame und keine Kopie auf der Transfer-Queue einen
  wiederverwendeten Bereich liest. Modelle und Terrain teilen die Blöcke.

## Konsequenzen

- Chunk-Grenzen sind ohne Nachbearbeitung geschlossen; der Test vergleicht eine Kugel über acht
  Chunks mit dem Netz eines einzigen Gitters.
- Pinsel und Strahltest arbeiten auf dem Store und kennen keine Chunk-Grenzen.
- Eckpunkte an Grenzen liegen doppelt vor (je einer pro Chunk); bei Chunks mit 32³ Samples ist
  das ein kleiner Anteil.
- Änderungen am Terrain gehen beim Speichern und beim Spielen im Editor verloren, bis R3.4 Deltas
  speichert.
- Das Terrain nutzt nur die Position seiner Entity.
- Für LOD (R3.3) muss die Randschicht Nachbarn anderer Stufen lesen; das Format von `ChunkCoord`
  hat die Stufe schon.
- Verschnitt im Geometriepuffer bleibt im Blick: `GeometryStats` zeigt belegten Speicher und
  freie Bereiche. Ein Netz größer als ein Block bekommt einen eigenen Block.

## Erwogene Alternativen

- **Chunks mit 33³ Samples, die sich die Grenzschicht teilen** (wie in D1): Jedes Sample an der
  Grenze läge doppelt vor und müsste bei Änderungen in mehreren Chunks geschrieben werden;
  Normalen an der Grenze brauchen trotzdem Nachbarn.
- **Netze nachträglich verschweißen:** braucht nach jeder Änderung auch die Netze der Nachbarn.
- **Chunks als eigene Entities:** viele Entities ohne Nutzen für den Editor; Streaming in R3.3
  verwaltet Chunks ohnehin selbst.
- **Ein Puffer pro Netz behalten:** einfach, aber viele kleine Allokationen und kein Weg zum
  indirekten Zeichnen in R3.5.
- **VMA Virtual Blocks** statt eigenem `RangeAllocator`: bindet die Verwaltung an das
  Vulkan-Backend; der eigene ist klein und ohne GPU testbar.
