# 0022: Geometrie per Vertex Pulling über Buffer Device Address

- **Status:** vorgeschlagen
- **Datum:** 2026-11-01

## Kontext

Bisher zeichnete die RHI nur ein Dreieck mit festen Daten im Shader. Für das Voxel-Terrain in D1
und alle Meshes ab R1 braucht sie Puffer. Klassisch beschreibt jede Pipeline ein Vertex-Layout
(Attribute, Bindings), und die Anwendung bindet Vertex-Buffer. Die Mindestanforderung
([ADR 0017](0017-platform-baseline-feature-tiers.md)) enthält Buffer Device Address, und der
Mesh-Shader-Pfad (REND-13) liest Geometrie ohnehin selbst aus Puffern.

## Entscheidung

- Pipelines haben keine Vertex-Attribute. Der Vertex-Shader liest seine Eckpunkte über einen
  Zeiger (Buffer Device Address) aus einem Puffer, den Index liefert `SV_VertexID`.
- Die Adressen und kleine Daten pro Draw kommen über Push Constants; jede Pipeline bietet dafür
  128 Bytes für alle Stufen.
- Indexpuffer bleiben klassisch gebunden (`bind_index_buffer`, 32 Bit), damit der Post-Transform-
  Cache der GPU greift.
- Jeder `Buffer` der RHI hat eine Geräteadresse.

## Konsequenzen

- Keine Vertex-Layouts in der RHI und kein Abgleich zwischen Layout und Shader. Formate der
  Eckpunkte legen Shader und C++-Struct gemeinsam fest (`voxel::Vertex`, 32 Bytes).
- Vertex- und Mesh-Shader-Pfad lesen dieselben Puffer; der Rückfallpfad ohne Mesh Shader braucht
  keine zweite Datenhaltung.
- Bindless (REND-06) fügt sich ein: Später stehen Adressen oder Indizes in einem Puffer pro Draw
  statt in Push Constants.
- Fehler beim Zugriff (falsche Adresse, zu kurzer Puffer) prüfen die Validation Layers nicht so gut
  wie Vertex-Attribute; GPU-Abstürze sind schwerer zu finden.
- Auf manchen älteren GPUs ist Vertex Pulling etwas langsamer als feste Attribute. Auf der
  Ziel-Hardware (RTX 20 und neuer, RDNA2 und neuer) ist der Unterschied vernachlässigbar.

## Erwogene Alternativen

- **Vertex-Attribute und gebundene Vertex-Buffer:** bewährt, aber Layouts in jeder Pipeline und ein
  zweiter Weg neben Mesh Shadern.
- **Storage Buffer über Descriptor-Sets:** braucht vor Bindless (REND-06) Descriptor-Verwaltung pro
  Draw, ohne Vorteil gegenüber Adressen.
