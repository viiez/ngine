# 0043: GPU-getriebenes Zeichnen mit Meshlets, Hi-Z in zwei Phasen und Rückfallpfad über indirekte Draws

- **Status:** angenommen
- **Datum:** 2026-11-12

## Kontext

Bis R3.4 zeichnete die CPU jedes Teilnetz und jeden Chunk mit einem eigenen Draw und prüfte nur
grob, ob er in eine Kaskade der Schatten fällt. Das endlose Terrain aus R3.3 hat tausende Chunks,
viele davon hinter Bergen oder von der Kamera abgewandt. Der Plan von R3 verlangt Culling auf der
GPU gegen Stumpf und Verdeckung, gezeichnet über Mesh Shader, wo die Stufe da ist, sonst über
Compute-Culling und indirekte Draws ([ADR 0017](0017-platform-baseline-feature-tiers.md):
optionale Stufen brauchen einen Rückfallpfad). Die Geometrie liegt seit R3.1 in gemeinsamen
Puffern ([ADR 0038](0038-terrain-chunks-shared-geometry.md)).

## Entscheidung

- **Meshlets** mit höchstens 64 Eckpunkten und 124 Dreiecken, mit Hüllkugel und Normalenkegel
  (`ngine.meshlet`). Der Indexpuffer wird umsortiert, sodass die Dreiecke eines Meshlets
  hintereinander liegen: Der Rückfallpfad zeichnet ein Meshlet als Bereich des Indexpuffers, der
  Mesh-Shader-Pfad liest dieselben Dreiecke über lokale Daten (Eckpunktliste, drei 8-Bit-Indizes je
  Dreieck). Terrain und Modelle aus Quellen nutzen den eigenen Builder (k-d-Teilung, deterministisch,
  ohne Fremdbibliothek); beim Kochen baut meshoptimizer sie (mit Vertex-Cache-Optimierung), die
  Hüllen rechnet in jedem Fall die Engine.
- **Instanzen und Gruppen:** Die CPU schreibt je Frame einen Instanzpuffer (Matrix, Adressen der
  Eckpunkte und Meshlets, Material, Kennung) und Gruppen zu 32 Meshlets einer Instanz. Eine Gruppe
  ist eine Arbeitsgruppe des Task- bzw. Compute-Shaders.
- **Prüfen** je Meshlet gegen die sechs Ebenen der Sicht, den Normalenkegel (nur Kamera und
  Auswahl, einseitige Materialien) und das Hi-Z. Verdeckung in **zwei Phasen**: Phase 1 prüft gegen
  das Hi-Z des letzten Frames (mit dessen Matrix) und zeichnet, was dort sichtbar war; aus der
  Tiefe danach entsteht das Hi-Z dieses Frames; Phase 2 zeichnet, was Phase 1 verworfen hat und
  jetzt sichtbar ist. Das Hi-Z speichert bei umgekehrter Tiefe das Minimum je Texel.
- **Zwei Pfade:** Mit Mesh Shadern sind Prüfen und Zeichnen ein Draw (Task-Shader startet je
  sichtbarem Meshlet einen Mesh-Shader). Ohne schreibt ein Compute-Shader `DrawIndexedCommand`s
  mit der Instanz als `first_instance`, gezeichnet mit `draw_indexed_indirect_count()` je Kategorie
  und Geometrieblock. Dafür werden **Multi Draw Indirect und Draw Indirect Count Pflicht**; beide
  sind Kern von Vulkan 1.2 und auf allen GPUs der Baseline vorhanden.
- **Durchsichtiges** bleibt auf der CPU, von hinten nach vorn sortiert, als einzelne Draws mit
  derselben Instanz.
- **Transiente Texturen** des Render-Graphs werden nach ihrem letzten Pass an den Vorrat
  zurückgegeben (TD-16); echtes Aliasing verschiedener Formate über VMA bauen wir nicht.

## Konsequenzen

- Was hinter Bergen oder in einer Höhle verdeckt ist, kostet nur noch das Prüfen seiner Meshlets.
  Die Zahl der Draw-Aufrufe hängt von Kategorien und Blöcken ab, nicht mehr von der Szene.
- Beide Pfade teilen Puffer und Shader-Funktionen; die CI vergleicht ihre Bilder über lavapipe.
- Statistik kommt von der GPU und damit einige Frames verspätet (TD-45). Schatten prüfen nicht auf
  Verdeckung (TD-44); Durchsichtiges wird nicht auf der GPU geprüft (TD-43).
- Gekochte Modelle haben ein neues Format (Version 2 mit Meshlets); alte Einträge im Cache werden
  neu gekocht.
- Meshlet-Daten brauchen rund ein Viertel bis ein Drittel zusätzlichen Geometriespeicher.

## Erwogene Alternativen

- **Culling je Instanz statt je Meshlet:** einfacher, aber große Chunks und Modelle würden ganz
  gezeichnet, sobald ein Teil sichtbar ist; Kegeltest ginge gar nicht.
- **Hi-Z nur aus dem letzten Frame (eine Phase):** spart einen Pass, lässt aber nach schnellen
  Drehungen Löcher, bis ein Frame später alles da ist.
- **Tiefen-Vorpass für Verdeckung:** zeichnet alles zweimal; die zweite Phase erreicht dasselbe mit
  weniger Arbeit.
- **Mesh Shader als Pflicht:** schließt GPUs der Baseline aus ([ADR 0017](0017-platform-baseline-feature-tiers.md)).
- **VMA-Aliasing für transiente Texturen:** Die großen Ziele überschneiden sich in ihrer
  Lebensdauer; der Gewinn rechtfertigt Speicherbarrieren und Platzierung nicht.
