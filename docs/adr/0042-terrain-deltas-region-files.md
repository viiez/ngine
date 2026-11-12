# 0042: Terrain-Änderungen als Deltas pro Chunk der Stufe 0, kodiert als Lauflängen, gespeichert in Regionen-Dateien neben der Szene

- **Status:** angenommen
- **Datum:** 2026-11-12

## Kontext

R3.4 macht Änderungen am Terrain dauerhaft (VOX-06, EDIT-08, TD-40). Die Grundwelt entsteht
deterministisch aus dem Generator ([ADR 0039](0039-deterministic-noise-rule-generator.md)) und
muss nie gespeichert werden; zu speichern ist nur, was Pinsel ändern. Ein endloses Terrain entlädt
Chunks hinter dem Beobachter und erzeugt sie später neu, auch in gröberen Stufen
([ADR 0041](0041-lod-octree-surface-nets-streaming.md)). Änderungen müssen das überleben, in der
Ferne sichtbar sein, mit der Szene gespeichert werden, sich im Editor rückgängig machen lassen und
in R5 übers Netz gehen (NET-06). Jobs erzeugen Chunks, während der Hauptthread malt.

## Entscheidung

- **Delta pro Chunk der Stufe 0:** Ein `ChunkDelta` hält die Samples, die von der Grundwelt
  abweichen. Kodierung: abwechselnd die Zahl unveränderter und geänderter Samples als Varint
  (LEB128), nach jeder Zahl geänderter Samples deren Werte (Dichte und Material, je ein Byte), in
  der Reihenfolge von `Chunk::index()`; unveränderte Samples am Ende fehlen. Ein Krater kostet so
  ungefähr seine geänderten Samples mal zwei Bytes. Deltas sind unveränderlich und werden geteilt
  (`shared_ptr<const ChunkDelta>`).
- **Grobe Stufen rechnen Deltas beim Erzeugen ein:** `apply_deltas()` übernimmt in einem groben
  Chunk jedes geänderte Sample der Stufe 0, das auf seinem Raster liegt. Nahe der Oberfläche gilt
  dessen Abstand in Schritten der Stufe; weiter weg bleibt der eigene Wert, wenn er auf derselben
  Seite liegt, sonst der größte Abstand der Stufe 0. Kleiner als ein Schritt verschwindet, wie
  beim Generator.
- **Momentaufnahmen statt Sperren:** `TerrainDeltas` gehört dem Hauptthread. Jobs bekommen beim
  Start die Tabelle als unveränderliche Momentaufnahme; ändert der Hauptthread ein Delta, während
  jemand die Tabelle hält, kopiert er sie (Kopie beim Schreiben). Der `Streamer` erzeugt nach
  einem Pinsel betroffene grobe Blätter neu und baut Chunks nach, deren Erzeugung mit einer
  älteren Momentaufnahme lief.
- **Undo über Deltas:** Ein Pinselstrich liefert pro Chunk das Delta vorher und nachher
  (`DeltaChange`); der Befehl im Editor hält beide und setzt sie zurück. Folgende Frames eines
  Strichs verschmelzen zu einem Schritt (erstes „vorher“, letztes „nachher“).
- **Regionen-Dateien:** 16 × 16 × 16 Chunks bilden eine Region, eine Datei `r.x.y.z.ndelta` pro
  Region mit Deltas, im Ordner `<Szene>.terrain` neben der Szene (Feld `Terrain::deltas`). Format
  Little Endian: „NGDR“, Version, Zahl der Chunks, je Chunk Lage in der Region (3 Bytes), Länge
  und Bytes des Deltas, nach Koordinaten sortiert. Gleiche Deltas ergeben gleiche Bytes. Beim
  Speichern verschwinden Dateien von Regionen ohne Deltas.

## Konsequenzen

- Gespeichert wird nur, was sich geändert hat; dieselben Bytes taugen für Undo, Dateien und später
  das Netz. Ändert sich der Generator, liegen die Deltas auf einer anderen Grundwelt (gewollt:
  Prüfsumme nach ADR 0039 hält ihn stabil).
- Ein Delta lohnt sich bei wenigen Änderungen pro Chunk; ein ganz umgegrabener Chunk kostet etwas
  mehr als der Chunk selbst (64 KiB). Für Spielwelten mit lokalen Kratern und Bauten ist das klein.
- Feine Änderungen unter einem Schritt einer Stufe sieht man aus der Ferne nicht.
- Grobe Chunks prüfen beim Erzeugen jedes Delta in ihrem Bereich (Schuld TD-41).
- Szenen mit Terrain bestehen aus mehreren Dateien; wer eine Szene kopiert, muss den Ordner
  mitnehmen.

## Erwogene Alternativen

- **Ganze Chunks speichern:** einfach, aber 64 KiB pro geändertem Chunk auch für einen Spatenstich
  und unbrauchbar fürs Netz.
- **Liste der Pinselstriche (Befehle) statt Samples:** sehr klein, aber Laden müsste alle Striche
  nachspielen, und das Ergebnis hängt vom Pinselcode jeder Version ab.
- **Deltas auf jeder Stufe getrennt pflegen:** grobe Stufen wären genauer, aber jede Änderung
  müsste in alle Stufen gerechnet und gespeichert werden.
- **Eine Datei pro Chunk oder eine für die ganze Welt:** viele kleine Dateien bzw. jedes Speichern
  schreibt alles neu; Regionen liegen dazwischen, wie in verbreiteten Voxel-Spielen.
- **Sperre um die Delta-Tabelle:** Jobs würden beim Erzeugen auf den Hauptthread warten.
