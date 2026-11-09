# 0041: LOD als ausgeglichener Baum von Chunks, Surface Nets über Stufengrenzen, Streaming mit Übernahme in einem Schritt

- **Status:** angenommen
- **Datum:** 2026-11-09

## Kontext

R3.3 macht das Terrain endlos: Sichtweite rund 2 km auf dem PC, mehrere Beobachter auf dem Server
(VOX-04, WORLD-07, WORLD-08). Mit Chunks einer Stufe wären das bei 16 m pro Chunk zehntausende
Chunks. In der Ferne reichen gröbere Chunks, doch wo Chunks verschiedener Stufen aneinanderstoßen,
passen ihre Netze nicht zusammen, und es entstehen Risse. Das Laden muss im Hintergrund laufen,
ohne dass ein Frame halb geladene Bereiche zeigt, und der Speicher muss begrenzt bleiben.

## Entscheidung

- **Baum der Chunks:** Ein Chunk der Stufe n deckt 32·2ⁿ Voxel mit 32³ Samples ab. Die Auswahl
  teilt einen Chunk, solange ein Beobachter näher als `detail` × seine Breite ist, und wird danach
  2:1 ausgeglichen: Benachbarte Blätter (auch über Kanten und Ecken) unterscheiden sich höchstens um
  eine Stufe. Der Generator füllt grobe Chunks direkt mit Samples im Abstand 2ⁿ; Stufe 0 bleibt
  bitgleich.
- **Surface Nets über Stufengrenzen statt Nahtstreifen:** Der `LodMesher` behandelt den Baum als ein
  Feld. Auf einer Fläche zwischen zwei Stufen gelten die Samples der groben Seite, Punkte der feinen
  Seite dazwischen werden linear interpoliert; jede Zelle hat damit genau einen Eckpunkt, den jedes
  Netz gleich berechnet. Jede Gitterkante gehört der Zelle, die in beiden Querrichtungen oben
  liegt, und verbindet die Eckpunkte der Zellen um sich (drei oder vier verschiedene) zu einem
  Viereck oder Dreieck. Weil der Baum ausgeglichen ist, liest das Netz eines Blatts nur die Blätter,
  die es berühren.
- **Streaming mit Übernahme in einem Schritt:** Der `Streamer` erzeugt fehlende Chunks einer neuen
  Auswahl im Job-System (Nahes zuerst, mit Budget) in eigenen Chunks und übernimmt die Auswahl erst,
  wenn alle da sind. Dann tauscht er Chunks und Netze in einem Aufruf aus und meshet nur neue
  Blätter und Blätter, deren Nachbarschaft sich geändert hat. Im `ChunkStore` liegen nur die
  Blätter der Auswahl.

## Konsequenzen

- Keine Risse zwischen Stufen und kein Frame mit Löchern; Tests ohne GPU prüfen das (keine offenen
  Kanten über mehrere Stufen).
- Der Mesher für den Baum ist aufwendiger als der für eine Stufe (rund 1,7-mal so teuer pro Netz)
  und eine zweite Umsetzung von Surface Nets neben `Mesher`, der für Terrains fester Größe bleibt.
- Eine Übernahme meshet viele Netze auf einmal im Hauptthread (parallel im Job-System) und lädt sie
  im selben Frame hoch; ein Budget für Uploads pro Frame fehlt noch (Schuld TD-39).
- Pinsel wirken nur auf Blätter der Stufe 0; Änderungen gehen beim Entladen verloren, bis R3.4 sie
  als Deltas speichert (Schuld TD-40).
- Ein Blatt, dessen Nachbarschaft sich ändert, wird ganz neu gemeshed, auch wenn sich nur ein Rand
  ändert.

## Erwogene Alternativen

- **Nahtstreifen (Transvoxel, Röcke):** eigene Tabellen oder überlappende Geometrie pro Seite und
  Stufenkombination; Röcke verdecken Risse nur und zeigen sich an Höhlen und Überhängen.
- **Netze der feinen Seite an grobe Kanten anpassen (Eckpunkte einrasten):** braucht ein erneutes
  Meshing der feinen Seite bei jeder Änderung der Nachbarn und lässt T-Kreuzungen übrig.
- **Clipmaps oder Ringe fester Stufen um eine Kamera:** einfacher, aber nur für einen Beobachter;
  der Server braucht mehrere.
- **Neue Chunks sofort zeigen:** kürzere Wartezeit, aber Frames mit Löchern und kurzzeitig nicht
  passenden Nähten.
