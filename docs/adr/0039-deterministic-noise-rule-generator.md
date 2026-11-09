# 0039: Eigenes deterministisches Rauschen und ein Generator aus Regeln

- **Status:** angenommen
- **Datum:** 2026-11-09

## Kontext

R3.2 bringt die prozedurale Grundwelt (VOX-05, VOX-09). Später erzeugen Server und Clients
dieselben Chunks aus einem Startwert und übertragen nur Änderungen als Deltas (R3.4, R5). Das geht
nur, wenn derselbe Startwert auf jeder Plattform, mit jedem Compiler und in jedem Build-Typ
bitgleiche Samples ergibt. Fertige Bibliotheken (FastNoise2, libnoise) nutzen SIMD-Pfade je nach
CPU, `std::floor`, Tabellen aus `std::mt19937` oder Funktionen wie `sin`, deren Ergebnis die
Standardbibliothek nicht festlegt. Compiler dürfen außerdem `a * b + c` zu einer FMA
zusammenziehen, was das Ergebnis im letzten Bit ändert.

## Entscheidung

- **Eigenes Rauschen** in `ngine.voxel:noise`: Simplex in 2D und 3D nach Gustavson. Gradienten
  kommen aus einem Hash der Gitterpunkte mit dem Startwert (SplitMix64), nicht aus einer
  Permutationstabelle; Startwerte werden mit `mix_seed()` gemischt. Darauf fBm, Ridged-Rauschen und
  Domain Warping.
- **Nur exakt festgelegte Rechnungen:** `f64`, nur `+ − × ÷` und `sqrt` (IEEE 754 schreibt sie
  korrekt gerundet vor). Eigenes `floor_to_int()` und `smoothstep()`, keine Funktionen aus
  `<cmath>` außer `sqrt`. `noise.cpp` und `generator.cpp` bauen mit `-ffp-contract=off` (Clang,
  GCC); MSVC zieht unter seiner Vorgabe `/fp:precise` ohnehin nichts zusammen. Die Option gilt
  nur für diese Dateien: Andere Optionen für das ganze Ziel ließen CMake die importierten Module
  ein zweites Mal übersetzen, was unter MSVC zu zwei Fassungen von `ngine.math` führte.
- **Generator aus Regeln** in `ngine.voxel:generator`: Höhe aus verzerrten Hügeln und Felsrücken
  (Ridged-Rauschen unter einer Maske, Terrassen für Felswände), Überhänge aus 3D-Rauschen je nach
  Steilheit, Höhlen als Tunnel (Nullstellen zweier Rauschfelder) und Kavernen mit Schutz unter der
  Oberfläche und Grundgestein. Schichten Gras, Erde, Fels, Sand nach Tiefe, Steilheit und Höhe.
  3D-Werte entstehen auf einem globalen Gitter mit Abstand 4 und werden trilinear interpoliert; das
  Gitter hängt nicht am Chunk, also sind Chunks unabhängig voneinander und in jeder Reihenfolge
  gleich. `sample()` liefert für jedes Sample dasselbe wie `fill()`.
- **Prüfsumme** (FNV-1a über alle Chunks) mit fest eingetragenem Wert im Test; die CI prüft sie
  unter Linux (Clang) und Windows (MSVC).

## Konsequenzen

- Die Welt ist reproduzierbar: Server und Clients können sie aus dem Startwert erzeugen,
  Speicherstände brauchen nur Deltas.
- `f64` und das Verbot von FMA kosten Zeit: rund 1,1 ms pro Chunk seriell im Release-Build,
  rund 0,3 ms parallel mit vier Threads. Für R3.3 (Streaming) reicht das; SIMD käme nur mit
  denselben Rechenschritten in Frage.
- Jede Änderung an Rauschen oder Regeln ändert die Prüfsumme und damit gespeicherte Welten; der
  Test zeigt das sofort. Formatversionen für Welten kommen mit den Deltas (R3.4).
- Neuer Code in `ngine.voxel`, der Samples berechnet, muss sich an dieselben Regeln halten.

## Erwogene Alternativen

- **FastNoise2 oder libnoise:** schnell bzw. bekannt, aber nicht bitgleich über Plattformen
  zugesichert; hinter einer Schnittstelle bliebe das Problem.
- **Festkomma-Arithmetik:** sicher deterministisch, aber umständlich und langsamer zu entwickeln;
  mit `f64` ohne FMA ist dasselbe Ziel erreicht (geprüft in Debug, Release und mit `-march=native`).
- **Höhenkarte allein:** einfacher und schneller, aber ohne Höhlen und Überhänge (VOX-09).
