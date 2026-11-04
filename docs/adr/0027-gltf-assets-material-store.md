# 0027: glTF über fastgltf, Bilder über stb_image, Materialien als Feld im GPU-Speicher

- **Status:** vorgeschlagen
- **Datum:** 2026-11-04

## Kontext

R1.3 bringt die ersten echten Modelle: glTF 2.0 ist das Austauschformat der Werkzeuge (Blender,
Khronos-Beispiele) und enthält Geometrie, PBR-Materialien nach Metallic-Roughness, Bilder und eine
Hierarchie. Eine Asset-Pipeline mit eigenem Laufzeitformat, KTX2 und Streaming kommt erst in R2
(ASSET-03 bis ASSET-05); bis dahin liest die Engine glTF direkt.

Der Renderer arbeitet bindlos ([ADR 0026](0026-render-graph-bindless.md)): Texturen und Sampler
haben feste Indizes im gemeinsamen Descriptor-Satz, Geometrie liest der Shader über Adressen
([ADR 0022](0022-vertex-pulling-bda.md)). Materialien brauchen einen Platz, der zu diesem Weg passt.

## Entscheidung

**Neues Modul `ngine.asset` in der Ressourcen-Schicht.** Es lädt Dateien in reine CPU-Daten
(`asset::Model` mit Meshes, Materialien, Texturen, Bildern und Knoten) und kennt weder GPU noch
Welt. Ein Eckpunkt hat ein festes Format von 48 Bytes (Position, Normale, Tangente mit Vorzeichen,
eine UV-Koordinate), Indizes sind 32 Bit, jedes Teilnetz ist eine Dreiecksliste. Fehlende
Normalen und Tangenten werden erzeugt (Tangenten pro Dreieck gemittelt, kein MikkTSpace).

**fastgltf für glTF, stb_image für Bilder**, jeweils nur in einem Adapter (`gltf_fastgltf.cpp`,
`image_stb.cpp`). Vor dem Lesen prüft der Adapter jeden Accessor gegen seine Buffer View und den
geladenen Puffer, weil fastgltf beim Iterieren keine Grenzen prüft; fehlerhafte Dateien liefern
eine Meldung statt eines Absturzes.

**Der Renderer besitzt Meshes und Materialien.** `MeshStore` legt die Teilnetze eines Meshes in
einen Eckpunkt- und einen Indexpuffer im GPU-Speicher. `MaterialStore` hält alle Materialien als
Feld von `GpuMaterial` (96 Bytes, nur 16-Byte-Felder) in einem Puffer; ein Material enthält
Faktoren und die bindlosen Indizes seiner Texturen und Sampler. Ein Draw bekommt über Push
Constants die Adressen von Eckpunkten, Frame-Daten und Materialfeld und den Index seines Materials.
Texturen bekommen beim Hochladen alle Mip-Stufen, die die GPU per Blit erzeugt; Grundfarbe und
Emission sind sRGB, alle anderen Texturen linear.

**Modelle werden Entities.** `render::spawn_model()` legt pro Knoten eine Entity mit `Transform`
und, falls ein Mesh daran hängt, `MeshRenderer` an. `MeshRenderer` verweist über Indizes in den
`MeshStore` und `MaterialStore`; diese gelten nur für einen Programmlauf, darum ist die Komponente
`transient` und wird nicht in Szenen gespeichert, bis Assets in R2 feste Kennungen bekommen.

## Konsequenzen

- glTF-Dateien lassen sich ohne Vorverarbeitung laden; Tests erzeugen kleine Dateien selbst und
  prüfen den Import ohne GPU.
- Bilder werden beim Laden dekodiert und auf der GPU verkleinert: einfach, aber langsam für große
  Texturen und ohne Kompression im Speicher. KTX2 mit Basis Universal folgt mit der
  Asset-Pipeline (R2).
- Ein Materialwechsel ist eine Zahl in den Push Constants, kein Descriptor-Wechsel. Ein falscher
  Index zeigt sich als falsches Bild, nicht als Fehler der Validation Layers.
- Nicht unterstützt und mit Warnung übergangen: Punkte und Linien, Skins, Animationen, Morph
  Targets, weitere UV-Sätze, dünn besetzte Accessoren, komprimierte Puffer, Erweiterungen wie
  `KHR_texture_transform`. Durchsichtige Materialien werden nicht sortiert (TD-20).
- Tangenten ohne MikkTSpace können an UV-Nähten leicht von der Normalenkarte abweichen, die ein
  Werkzeug gebacken hat. Mit der Asset-Pipeline kommen bei Bedarf MikkTSpace-Tangenten.

## Erwogene Alternativen

- **cgltf oder tinygltf:** cgltf ist klein, liefert aber rohe C-Strukturen ohne Hilfen für
  Accessoren; tinygltf bringt eigene JSON- und Bildbibliotheken mit und ist langsamer. fastgltf
  ist schnell (simdjson), modern (C++17/20) und liefert typsichere Accessor-Hilfen.
- **Assimp:** liest viele Formate, ist aber groß und baut lange; glTF reicht als Austauschformat.
- **Materialien in Push Constants statt im Puffer:** 128 Bytes reichen nicht für Faktoren, zehn
  Indizes und die Matrix des Draws.
- **Ein Puffer für alle Meshes:** spart Bindungen und passt später zum GPU-getriebenen Zeichnen,
  braucht aber eine Verwaltung freier Bereiche. Kommt mit dem Culling auf der GPU (R3).
