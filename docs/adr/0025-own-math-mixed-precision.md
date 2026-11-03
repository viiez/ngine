# 0025: Eigene Mathematik in `ngine.math`, Weltlage mit gemischter Genauigkeit

- **Status:** vorgeschlagen
- **Datum:** 2026-11-03

## Kontext

R1 braucht Vektoren, Matrizen und Quaternionen für Transforms, Kamera und Renderer (CORE-10).
Bisher hatten Voxel-Modul und Sandbox eigene kleine Typen (TD-10). Weltpositionen sind 64 Bit,
gezeichnet wird relativ zur Kamera in 32 Bit ([ADR 0012](0012-64bit-world-coordinates.md)).
Fremdbibliotheken sollen hinter Engine-Schnittstellen stecken
([ADR 0009](0009-third-party-behind-interfaces.md)); bei Mathematik-Typen, die in jeder
Komponente und jeder Funktionssignatur vorkommen, geht das nur mit eigenen Typen oder mit
Typ-Aliasen.

Die erste Fassung von R1.1 nutzte GLM hinter Aliasen. Dabei zeigte sich, dass GLM schlecht zu uns
passt:

- Das Wichtigste für große Welten, eine Weltlage mit 64-Bit-Verschiebung, hat GLM nicht; sie war
  ohnehin eigener Code.
- Damit die Reflection die Felder sieht, brauchte es `GLM_FORCE_XYZW_ONLY`, was die SIMD-Pfade von
  GLM abschaltet. Komponenten mussten außerdem `GLM_FORCE_CTOR_INIT` setzen, damit sie nicht mit
  Zufallswerten starten.
- Mit C++-Modulen mussten die Operatoren im Namensraum `glm` exportiert werden; Namen in
  camelCase, der Konstruktor von `glm::quat` mit w zuerst und `epsilonEqual` mit „kleiner“ statt
  „kleiner gleich“ brauchten jeweils eine eigene Hülle.
- Genutzt wurde nur ein kleiner Teil: rund 30 Funktionen, Quaternionen, Umkehrmatrizen und drei
  Projektionen.

## Entscheidung

- **Eigene Implementierung** in `ngine.math` ohne Fremdbibliothek, skalar geschrieben. SIMD kommt
  erst dort, wo eine Messung es verlangt (z. B. Culling, Propagation großer Szenen), und dann
  gezielt für diese Stelle.
- **Typen:** `Vec2`, `Vec3`, `Vec4`, `IVec2`, `IVec3`, `DVec3` als Aggregate über Vorlagen
  (`TVec2<T>` …), `Mat3`, `Mat4`, `DMat3` mit Spalten, `Quat` (Felder x, y, z, w), `Affine`.
  Standardwerte sind Nullvektor, Einheitsmatrix und Einheitsquaternion; Komponenten starten damit
  ohne weitere Angaben definiert. Die Reflection beschreibt die Felder direkt.
- **Konventionen:** rechtshändig, y oben, Kamera blickt entlang -z, Spaltenvektoren, Matrizen
  spaltenweise im Speicher (`m[spalte]`), Tiefe 0 bis 1. Zusätzlich `perspective_reverse_z()` mit
  umgekehrter Tiefe und unendlicher Ferne für große Welten (ab R1.3).
- **Schnittstelle:** Erzeugen mit allen Komponenten (`Vec3{1, 2, 3}`) oder über den
  Standardkonstruktor; Umwandlungen und Einheitswerte über Funktionen (`to_f32()`, `to_f64()`,
  `xyz()`, `extend()`, `quat_identity()`, `mat3_identity()`, `mat4_identity()`, `to_mat3()`,
  `to_mat4()`, `to_quat()`); Operatoren als versteckte Freunde der Typen, damit sie ohne
  Exporttricks in jedem Modul gefunden werden. Namen in snake_case.
- **GLM als Orakel:** GLM bleibt nur als Abhängigkeit der Tests. `tests/math/math_oracle_tests.cpp`
  vergleicht Produkte, Umkehrmatrizen, Determinanten, Quaternionen, Blick- und Projektionsmatrizen
  mit zufälligen Eingaben gegen GLM. Bewusste Abweichung: `normalize()` des Nullvektors liefert den
  Nullvektor statt NaN.
- **Gemischte Genauigkeit:** `math::Affine` hält Drehung, Skalierung und Scherung als `Mat3` in
  32 Bit und die Verschiebung als `DVec3` in 64 Bit. Verkettung und Umkehrung rechnen die
  Verschiebung in 64 Bit; `relative_to(origin)` liefert die `Mat4` für die GPU, deren Verschiebung
  erst nach der Differenz in 64 Bit auf 32 Bit gerundet wird.
- **Transforms:** `world::Transform` (lokal: `DVec3` Position, `Quat` Drehung, `Vec3`
  Skalierung) wird gespeichert; `world::WorldTransform` (ein `Affine`) wird von
  `update_transforms()` entlang der Hierarchie berechnet, kommt automatisch mit `Transform` dazu
  und ist als `transient` markiert, damit Szenen ihn nicht speichern.

## Konsequenzen

- Volle Kontrolle über Typen, Namen und Genauigkeit; keine Schalter, die für alle Einbindungen
  gleich sein müssen, kein Durchgriff auf eine Bibliothek.
- Rund 700 Zeilen eigener Code, die wir pflegen. Die Vergleichstests gegen GLM fangen Fehler in
  Formeln, die sonst erst im Bild auffielen.
- Speicherlayout wie in GLSL und Slang (Felder x, y, z, w, Matrizen spaltenweise): Matrizen
  lassen sich direkt in Push Constants und Puffer kopieren.
- Eine Weltlage belegt 64 Bytes (36 für die Matrix, 24 für die Verschiebung, Rest Ausrichtung)
  statt 128 für eine `DMat4`. Die GPU sieht nur kamerarelative 32-Bit-Matrizen.
- Die Propagation läuft über eine Abfrage von flecs mit Cascade (Eltern vor Kindern) und
  vorerst in einem Thread.

## Erwogene Alternativen

- **GLM hinter Aliasen:** erste Fassung von R1.1, aus den oben genannten Gründen ersetzt. Bleibt
  als Testorakel.
- **DirectXMath:** SIMD von Anfang an, aber linkshändig, zeilenweise und an Windows-Konventionen
  ausgerichtet; unter Linux nur als Header-Kopie.
- **Weltlage als `DMat4`:** einfacher, aber doppelt so groß und für die GPU ohnehin umzurechnen.
- **Weltlage als Position, Drehung, Skalierung:** kleiner, aber nicht abgeschlossen unter
  Verkettung, sobald ungleichmäßige Skalierung auf Drehung trifft (Scherung).
