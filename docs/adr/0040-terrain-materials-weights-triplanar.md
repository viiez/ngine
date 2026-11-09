# 0040: Terrain-Materialien als Gewichte je Eckpunkt mit Palette pro Chunk, triplanar gezeichnet

- **Status:** angenommen
- **Datum:** 2026-11-09

## Kontext

Bis R3.1 trug jeder Eckpunkt des Terrains ein Material, und der Shader färbte danach. Zwischen
zwei Materialien entstand so eine harte Kante quer durch die Dreiecke. R3.2 verlangt mehrere
Materialien pro Oberfläche mit weichen Übergängen (VOX-07) und Texturen aus der Asset-Pipeline.
Surface-Nets-Netze haben keine UV-Koordinaten, und Felswände, Überhänge und Höhlen sind in alle
Richtungen geneigt.

## Entscheidung

- **Gewichte je Eckpunkt:** Der Mesher berechnet für jeden Eckpunkt aus den festen Ecken seiner
  Zelle Gewichte (je Ecke `1 / (d + 0,25)²` nach dem Abstand zum Eckpunkt) und packt sie in acht
  Bytes mit Summe genau 255. `voxel::Vertex` bleibt 32 Bytes groß (`weights_low`,
  `weights_high`).
- **Palette pro Chunk:** Die acht Gewichte gehören zu den acht Plätzen einer Palette des Netzes
  (`Mesh::palette`). Ein Chunk mit mehr als acht Materialien legt weitere auf Platz 0. Die Palette
  kommt als zwei Wörter in die Push Constants.
- **Materialtabelle:** `render::TerrainMaterial` (Farbe, Roughness, Meter pro Wiederholung,
  Stärke der Normalen, Grundfarbe und Normalen als `AssetId`). Der Renderer schreibt sie jeden Frame
  als `GpuTerrainMaterial` in einen Puffer; Texturen lädt der Ressourcen-Manager (`ColorTexture`
  sRGB, `DataTexture` linear), bis dahin gilt die Farbe allein.
- **Triplanar im Shader:** Jedes Material wird aus drei Projektionen entlang der Achsen gelesen,
  gewichtet nach der Normale (Potenz 4), Normalen über das Verfahren UDN. Die Texturen haften an
  der Lage im Terrain, nicht an der kamerarelativen Lage. Der Fragment-Shader mischt alle Plätze mit
  Gewicht ab 1/64.

## Konsequenzen

- Übergänge verlaufen über ein bis zwei Zellen ohne Kante; Felswände und Höhlendecken zeigen
  unverzerrte Texturen.
- Bis zu acht Materialien mal drei Projektionen mal zwei Texturen pro Pixel; an den meisten Stellen
  sind es ein oder zwei Materialien. Bei Bedarf hilft später, nur die zwei stärksten Projektionen
  zu lesen.
- Mehr als acht Materialien in einem Chunk fallen auf Platz 0 zurück (Schuld TD-38).
- Die Tabelle steht im Code und wird über `Renderer::set_terrain_materials()` gesetzt, Texturen
  sind ungekochte Bilder (Schuld TD-37).

## Erwogene Alternativen

- **Ein Material je Eckpunkt mit Überblenden im Shader nach Abstand:** braucht Nachbarn im
  Shader oder doppelte Eckpunkte an Grenzen.
- **Gewichte für alle Materialien als Textur oder Puffer je Chunk:** mehr Speicher, ein Zugriff
  mehr pro Pixel, keine Grenze bei acht.
- **Texture Arrays statt bindloser Einzeltexturen:** gleiche Größe und Format für alle Materialien
  erzwungen; Bindless ist ohnehin vorhanden ([ADR 0026](0026-render-graph-bindless.md)).
