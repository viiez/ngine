# 0045: Physik mit Jolt in doppelter Genauigkeit, die Welt als Quelle der Wahrheit

- **Status:** angenommen
- **Datum:** 2026-11-14

## Kontext

R4 braucht starre Körper, Kollision und Abfragen für Kisten, die Spielfigur (R4.3), Skripte (R4.4)
und später die Replikation (R5). Die Welt rechnet Positionen in 64 Bit
([ADR 0012](0012-64bit-world-coordinates.md)), Szenen und der Spielmodus des Editors sichern die
Welt über die Reflection ([ADR 0032](0032-play-in-editor-binary-snapshot.md)), und die feste
Simulation mit eigenem Job-System steht seit R4.1 ([ADR 0044](0044-game-loop-phases-fixed-step-commands.md)).
Der vcpkg-Port von Jolt baut nur mit einfacher Genauigkeit und gibt Befehlssatz-Schalter wie
`-mavx2` an seine Nutzer weiter. Moduldateien von Clang lassen sich nur mit denselben Schaltern
laden, mit denen sie gebaut sind.

## Entscheidung

- **Jolt 5.6 hinter `ngine.physics`.** Nur die Adapter-Datei `jolt_world.cpp` kennt Jolt; die
  Schnittstelle besteht aus eigenen Typen (`PhysicsWorld`, `RayHit`, `ShapeHit`, `ContactBegan`, …)
  und einem Pimpl.
- **Eigener Overlay-Port** `cmake/vcpkg-ports/joltphysics`, eingebunden über
  `vcpkg-configuration.json`: eine Kopie des offiziellen Ports mit `DOUBLE_PRECISION=ON`, ohne
  Befehlssatz-Schalter (`USE_SSE4_1` bis `USE_AVX512`, `USE_FMADD` aus, Jolt rechnet mit SSE2) und
  ohne Gleitkomma-Ausnahmen, die nur für die Konfigurationen Debug und Release gesetzt würden. RTTI
  ist an, weil der Job-Adapter von einer Klasse der Bibliothek erbt. `ngine_physics` leert die
  weitergegebenen Schalter von `Jolt::Jolt` (`-pthread`, `-msse2`, `-mfpmath=sse`) und behält die
  Definitionen. `VerifyJoltVersionID()` prüft beim Start, dass Bibliothek und Adapter zusammenpassen.
- **Positionen in `f64`** (`RVec3`), passend zu `Transform::position`; Drehungen, Formen und
  Geschwindigkeiten bleiben `f32`. Ein verschiebbarer Ursprung der Physik-Welt entfällt.
- **Die Welt ist die Quelle der Wahrheit.** Körper entstehen aus den Komponenten `RigidBody` und
  `Collider`; die Physik-Welt hält nur den Zustand der Simulation. Vor jedem Schritt gleicht sie ab:
  neue Körper, weggefallene, geänderte Werte (neu angelegt), von außen versetzte Transforms und
  gesetzte Geschwindigkeiten. Nach dem Schritt schreibt sie Lage und Geschwindigkeit aktiver
  dynamischer Körper zurück. Die Zuordnung Entity zu Körper liegt in der Physik-Welt, nicht in
  einer Komponente. Bewegliche Körper bekommen `Interpolated`.
- **Stufe der festen Simulation:** `Simulation::add_fixed_stage()` führt den Schritt nach den
  Systemen jedes festen Schritts und ihren Befehlspuffern im Hauptthread aus; `ngine.world` bleibt
  ohne Physik.
- **Reproduzierbar auf derselben Plattform und im selben Build:** Körper entstehen in der
  Reihenfolge der Kennungen, Kontakte werden gesammelt und sortiert veröffentlicht, und `clear()`
  beginnt mit einer frischen Jolt-Welt. Dann ist ein Lauf bitgleich, mit 1 wie mit 16 Threads.
  `CROSS_PLATFORM_DETERMINISTIC` bleibt aus; in R5 bestimmt der Server die Physik.
- **Jolts Jobs laufen auf `ngine.jobs`** (Adapter auf `JobSystemWithBarrier`). Nach jedem Schritt
  wartet die Physik, bis alle eingereihten Jobs gelaufen sind, damit keiner einen Zeiger in den
  Adapter behält. Unter ThreadSanitizer rechnet Jolt nur im wartenden Thread, weil die Bibliothek
  nicht instrumentiert ist (TD-47).
- **Ebenen fest im Code** (`Static`, `Moving`, `Character`, `Debris`, `Sensor`) mit einer
  symmetrischen Tabelle `layers_collide()`; die Breitenphase kennt unbewegt und bewegt.
- **Formen aus Modellen** über eine `ShapeSource` der Anwendung (Dreiecke zu Modell und Mesh); die
  Physik lädt keine Assets selbst und merkt sich gebaute Formen nach Modell, Mesh und Skalierung.

## Konsequenzen

- Spielmodus, Szenen, Prefabs und Inspector brauchen keinen Sonderweg: Stoppen stellt die Welt aus
  der Sicherung her, und die Physik verwirft ihre Körper. Zweimal Spielen ergibt dasselbe (Test).
- Ein Stapel 10 000 km vom Ursprung verhält sich wie am Ursprung (unter 1 mm nach 5 s, Test).
- Ohne AVX2 und FMA ist Jolt langsamer als mit; 2 031 Körper kosten im Container (4 Kerne, Release)
  rund 8 ms pro Schritt. Ob sich die Befehlssätze lohnen, zeigt die Messung auf dem PC (TD-48).
- Der Overlay-Port muss bei einer neuen Baseline mit dem offiziellen Port abgeglichen werden.
- Ein Lauf ist nur im selben Build bitgleich: Debug und Release, Linux und Windows ergeben
  verschiedene Prüfsummen.
- Dreiecksnetze gibt es nur für unbewegte und kinematische Körper; ein dynamischer Körper mit Netz
  nimmt die konvexe Hülle. Mehrere Formen pro Körper (Compound) gibt es noch nicht.
