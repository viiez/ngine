# 0046: Terrain-Kollision aus den Chunk-Netzen und Spielfigur mit Jolts CharacterVirtual

- **Status:** angenommen
- **Datum:** 2026-11-14

## Kontext

R4.3 bringt Körper und eine Spielfigur auf das endlose Voxel-Terrain. Das Terrain ändert sich zur
Laufzeit (Graben, Bauen, [ADR 0042](0042-terrain-deltas-region-files.md)) und lädt sich um die
Beobachter nach ([ADR 0041](0041-lod-octree-surface-nets-streaming.md)); Netze entstehen in Jobs.
Die Physik ist Jolt hinter `ngine.physics`, die Welt die Quelle der Wahrheit
([ADR 0045](0045-physics-jolt-double-precision.md)). Für R5 muss die Figur auf dem Server
reproduzierbar laufen und sich vorhersagen lassen. Die Materialtabelle des Terrains stand im Code
(TD-37), Reibung und Schritte brauchen sie aber auch in der Physik und im Spiel.

## Entscheidung

- **Eine `MeshShape` pro Chunk der Stufe 0** aus dem Netz, das ohnehin für den Renderer entsteht
  (Positionen in Metern relativ zur Ecke des Chunks). Gebaut wird in Jobs, eingesetzt im Abgleich
  vor dem Schritt; ein neues Netz ersetzt die Form per `SetShape`, die alte bleibt bis dahin.
- **Nur in der Nähe von Ankern:** dynamische Körper, Figuren und `TerrainObserver` bekommen die
  Chunks im Radius von 24 m (`PhysicsSettings::terrain_radius`); Formen ohne Bedarf verschwinden
  nach 120 Schritten. Grobe Stufen haben keine Kollision.
- **Einfrieren statt Fallen:** Ein Körper oder eine Figur, unter der die Form noch fehlt, wird
  kinematisch gehalten und nicht zurückgeschrieben, bis die Form da ist. Mit
  `PhysicsSettings::wait_for_terrain` (Server, Tests, `--headless`) wartet der Abgleich stattdessen
  auf die Formen; dann ist das Ergebnis unabhängig von der Zahl der Threads.
- **Spielfigur mit `JPH::CharacterVirtual`** hinter eigenen Komponenten: `CharacterController`
  (gespeichert: Maße, Hang, Stufe, Sprung, Tempo, Beschleunigung, Schub) und `CharacterMotion`
  (transient: Wunsch `move`, `run`, `crouch`, `jump`; Ergebnis Geschwindigkeit, Boden, Normale,
  Material). Die Figur bewegt sich vor dem Schritt der Körper mit `ExtendedUpdate` (Treppen,
  Haften am Boden), hat einen inneren Körper auf der Ebene `Character` für Strahlen und Auslöser
  und meldet `CharacterLanded` und `CharacterStep`. Ducken tauscht die Kapsel, wenn oben Platz ist.
- **Materialtabelle als Asset** `world::TerrainMaterials` (`.terrain-materials`, JSON über die
  Reflection): Aussehen, Reibung, Rückprall, Art der Schritte. Jedes Dreieck einer Chunk-Form trägt
  das Material mit dem größten Gewicht an seinen Ecken als eigenes `PhysicsMaterial`; Reibung und
  Rückprall kommen beim Kontakt aus der Tabelle, `RayHit::material` und
  `CharacterMotion::ground_material` nennen es. Renderer und Physik lesen dieselbe Tabelle.
- **Austauschbar:** `CharacterVirtual`, `MeshShape` und `PhysicsMaterial` kennt nur
  `jolt_world.cpp`; die Schnittstelle besteht aus eigenen Typen (`TerrainChunkShape`,
  `CharacterController`, `RayHit`).

## Konsequenzen

- Graben ändert die Kollision ohne Sonderweg: Der Pinsel markiert das Netz, das neue Netz wird zur
  neuen Form, Körper in der Nähe wachen auf.
- Speicher und Bauzeit wachsen mit der Zahl der Anker, nicht mit der Sichtweite. Ferne Körper ohne
  Anker in der Nähe bleiben eingefroren; Geschosse oder Fahrzeuge weit weg brauchen später einen
  größeren Radius oder grobe Formen (TD-50).
- Die Figur steuert sich über Komponenten; Skripte (R4.4) und die Replikation (R5) setzen nur
  `CharacterMotion` und lesen das Ergebnis.
- Ohne Anbindung des Spiels an die Datei nehmen `--headless` und der Server die eingebaute Tabelle
  (TD-51).

## Erwogene Alternativen

- **Formen aller geladenen Chunks der Stufe 0:** einfacher, aber Speicher und Bauzeit wachsen mit
  der Sichtweite.
- **Höhenfeld statt Dreiecksnetz:** billiger, kann aber keine Höhlen und Überhänge.
- **Kollision direkt aus der Dichte** (eigene Form mit Abfragen auf die Samples): kein zweites
  Netz, aber eine eigene Jolt-Form mit allen Abfragen ist viel Arbeit und an Jolt gebunden.
- **Jolts `Character`** (echter starrer Körper): schlechter für genaue Steuerung und Vorhersage.
- **Eigene Figur aus Form-Abfragen:** volle Kontrolle, aber Treppen, Hänge und Schieben müssten
  neu entstehen.
