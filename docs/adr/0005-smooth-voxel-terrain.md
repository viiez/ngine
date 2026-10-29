# 0005: Glattes Voxel-Terrain mit Dichtefeld

- **Status:** angenommen
- **Datum:** 2026-10-23

## Kontext

Das Spiel braucht eine veränderbare Welt wie in Enshrouded: glatte Oberflächen, Höhlen, Bauen und
Abbauen. Der Rest der Welt (Figuren, Objekte, Gebäudeteile) besteht aus klassischen Meshes.

## Entscheidung

- Terrain als Dichtefeld mit Material pro Voxel, in Chunks gespeichert.
- Meshing über Surface Nets oder Dual Contouring; die Wahl fällt im Durchstich D1.
- LOD für die Ferne, prozedurale Grundwelt, Änderungen als Deltas.
- Voxel-Terrain ist ein eigenes System neben normalen Meshes.

## Konsequenzen

- Glatte Formen statt Blöcken, echte 3D-Strukturen statt Höhenkarte.
- Änderungen lösen neues Meshing, Kollision und Beleuchtung aus; das Job-System wird zentral.
- Vorberechnete Beleuchtung fällt weg, dynamische GI wird nötig.

## Erwogene Alternativen

- **Blockige Voxel (Minecraft):** einfacher, passt aber nicht zur Spielidee.
- **Höhenkarte:** keine Höhlen und Überhänge.
