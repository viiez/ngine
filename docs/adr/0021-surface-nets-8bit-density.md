# 0021: Surface Nets und 8-Bit-Dichte für das Voxel-Terrain

- **Status:** angenommen
- **Datum:** 2026-11-01

## Kontext

[ADR 0005](0005-smooth-voxel-terrain.md) legt glattes Voxel-Terrain mit Dichtefeld fest und lässt
offen, ob wir mit Surface Nets oder Dual Contouring meshen. Außerdem ist offen, wie viel Speicher
ein Sample kostet: Die Welt soll groß sein, Änderungen sollen sich speichern und übers Netz
verteilen lassen (VOX-06, NET-06). Der Durchstich D1 hat Surface Nets gebaut und gemessen.

## Entscheidung

- **Meshing mit Naive Surface Nets:** ein Eckpunkt pro Voxel mit Vorzeichenwechsel (Mittel der
  Kantenschnitte), ein Viereck pro Gitterkante mit Vorzeichenwechsel, Normalen aus dem Gradienten
  der Dichte.
- **Sample aus 2 Bytes:** Dichte als vorzeichenbehafteter Abstand zur Oberfläche in 8 Bit (positiv
  innen, 1/32 Voxel pro Schritt, geklemmt bei rund ±4 Voxeln) und 8 Bit Material, 0 ist Luft.
- **Bearbeiten über Abstände:** Pinsel verknüpfen Abstandsfelder (Maximum zum Auftragen, Minimum
  zum Abtragen, Mittelung zum Glätten) und schreiben das Ergebnis zurück.
- Das Voxel-System läuft ohne GPU (`ngine.voxel`); das Zeichnen gehört dem Renderer.

## Konsequenzen

- Einfacher, schneller Mesher ohne Gleichungssysteme: 0,45 ms für einen Chunk mit 32³ Voxeln im
  Release-Build auf einem Kern. Ein Chunk lässt sich bei jeder Änderung im selben Frame neu meshen;
  viele Chunks verteilen wir in R3 auf das Job-System.
- Das Netz ist geschlossen und richtig orientiert, solange die Oberfläche nicht den Rand berührt.
  Am Rand eines Chunks bleiben Kanten offen: Für nahtlose Chunks braucht der Mesher in R3 eine
  Randschicht der Nachbarn (eine Zelle für die Vierecke, eine weitere für die Normalen).
- Scharfe Kanten werden rund. Für Terrain gewollt; brauchen gebaute Strukturen scharfe Kanten,
  lässt sich Dual Contouring pro Material nachrüsten, weil Dichte und Gradient dafür reichen.
- 8 Bit reichen für die Form: Eine Kugel weicht weniger als 0,15 Voxel ab. Ein Chunk mit 33³
  Samples kostet 70 KiB; leere und volle Chunks lassen sich später als ein Wert speichern.
- Der Abstand ist nach ±4 Voxeln geklemmt. Pinsel mit großem Radius rechnen deshalb mit dem
  geklemmten Wert; das reicht, weil nur die Nähe der Oberfläche das Netz bestimmt.
- LOD ohne Risse (VOX-04) ist mit Surface Nets aufwendiger als mit Transvoxel und bleibt ein
  Risiko für R3.

## Erwogene Alternativen

- **Dual Contouring:** erhält scharfe Kanten, braucht aber Hermite-Daten (Normalen an den
  Kantenschnitten) und eine QEF-Lösung pro Voxel; teurer und anfälliger für Ausreißer, ohne dass
  Terrain davon profitiert.
- **Marching Cubes / Transvoxel:** gut erprobt, Transvoxel löst LOD-Risse. Dafür mehr Dreiecke,
  große Tabellen und schmale Dreiecke; die Eckpunkte liegen auf Kanten und lassen sich schlechter
  glätten.
- **Dichte als `f32` oder 16 Bit:** genauer, aber zwei- bis viermal so viel Speicher und Netzlast,
  ohne sichtbaren Gewinn im Durchstich.
