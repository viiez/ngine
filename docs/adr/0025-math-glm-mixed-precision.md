# 0025: Mathematik mit GLM hinter `ngine.math`, Weltlage mit gemischter Genauigkeit

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

## Entscheidung

- **GLM** (MIT, aus vcpkg, nur Header) ist die Grundlage. Das Modul `ngine.math` bindet GLM im
  globalen Modulfragment ein und exportiert Aliase in Engine-Schreibweise (`Vec3`, `DVec3`,
  `Mat4`, `Quat` …) und die Funktionen, die die Engine nutzt. GLM-Namen in camelCase bekommen eine
  Fassung in snake_case, die zugleich die Konventionen festlegt (`look_at` rechtshändig,
  `perspective` mit Tiefe 0 bis 1). Außerhalb von `ngine.math` bindet niemand GLM ein.
- **Schnittstelle statt Bibliothek:** Code außerhalb von `ngine.math` nutzt nur, was eine
  eigene Implementierung später ebenso liefern kann, damit ein Austausch auf `engine/math/`
  beschränkt bleibt:
  - Typen `Vec2`, `Vec3`, `Vec4`, `IVec2`, `IVec3`, `DVec3`, `Mat3`, `Mat4`, `DMat3`, `Quat`,
    `Affine`; Felder `x`, `y`, `z`, `w`; Matrizen spaltenweise mit `m[spalte]`.
  - Erzeugen mit allen Komponenten (`Vec3{1, 2, 3}`) oder über den Standardkonstruktor
    (Nullvektor, Einheitsmatrix, Einheitsquaternion). Einheitswerte und Umwandlungen über
    `quat_identity()`, `mat3_identity()`, `mat4_identity()`, `to_f32()`, `to_f64()`, `xyz()`,
    `extend()`; nicht über Konstruktoren von GLM (`Vec3{dvec}`, `Vec3{1.0f}`, `Quat{w, x, y, z}`).
  - Operatoren `+`, `-`, `*`, `/` (komponentenweise und mit Skalar), `==`, Matrix mal Vektor und
    Matrix, Quaternion mal Vektor und Quaternion, die Zuweisungsoperatoren.
  - Die exportierten Funktionen von `ngine.math`; keine Swizzles, keine weiteren GLM-Funktionen.
  - Die Tests unter `tests/math/` decken diesen Umfang ab und sind beim Wechsel die Abnahme.
- **Konventionen:** rechtshändig, y oben, Kamera blickt entlang -z, Spaltenvektoren, Matrizen
  spaltenweise im Speicher, Tiefe 0 bis 1. Zusätzlich `perspective_reverse_z()` mit umgekehrter
  Tiefe und unendlicher Ferne für große Welten (ab R1.3).
- **Schalter:** `GLM_FORCE_CTOR_INIT` (Standardwerte statt Zufall, Komponenten müssen definiert
  starten), `GLM_FORCE_XYZW_ONLY` (Felder ohne Unions, damit die Reflection sie beschreiben
  kann), `GLM_FORCE_DEPTH_ZERO_TO_ONE`.
- **Gemischte Genauigkeit:** `math::Affine` hält Drehung, Skalierung und Scherung als `Mat3` in
  32 Bit und die Verschiebung als `DVec3` in 64 Bit. Verkettung und Umkehrung rechnen die
  Verschiebung in 64 Bit; `relative_to(origin)` liefert die `Mat4` für die GPU, deren Verschiebung
  erst nach der Differenz in 64 Bit auf 32 Bit gerundet wird.
- **Transforms:** `world::Transform` (lokal: `DVec3` Position, `Quat` Drehung, `Vec3`
  Skalierung) wird gespeichert; `world::WorldTransform` (ein `Affine`) wird von
  `update_transforms()` entlang der Hierarchie berechnet, kommt automatisch mit `Transform` dazu
  und ist als `transient` markiert, damit Szenen ihn nicht speichern.

## Konsequenzen

- Schneller Start mit einer erprobten Bibliothek in Shader-Schreibweise; Quaternionen,
  Umkehrmatrizen und Projektionen müssen wir nicht selbst schreiben und testen.
- `GLM_FORCE_XYZW_ONLY` schaltet die SIMD-Pfade von GLM ab. Für Transforms fällt das nicht ins
  Gewicht; wo Mathematik zum Engpass wird, messen wir zuerst und optimieren dann gezielt.
- GLM-Typen sind über die Aliase überall sichtbar. Ein späterer Wechsel zu eigenen Typen bleibt
  ein Umbau von `engine/math/`, solange der übrige Code sich an die Schnittstelle oben hält; Feldnamen
  und Speicherlayout bleiben gleich, Szenen und GPU-Daten sind davon nicht betroffen.
- Damit Operatoren in importierenden Modulen gefunden werden, exportiert `ngine.math`
  using-Deklarationen im Namensraum `glm` (wie das Modul, das GLM selbst mitbringt).
- Eine Weltlage belegt 64 Bytes (36 für die Matrix, 24 für die Verschiebung, Rest Ausrichtung)
  statt 128 für eine `DMat4`. Die GPU sieht nur kamerarelative 32-Bit-Matrizen.
- Die Propagation läuft über eine Abfrage von flecs mit Cascade (Eltern vor Kindern) und
  vorerst in einem Thread.

## Erwogene Alternativen

- **Eigene kleine Typen:** volle Kontrolle und kein Durchgriff auf eine Bibliothek, aber mehr
  Code und Tests, bevor das Rendering beginnen kann. Bleibt möglich, wenn GLM stört.
- **GLM direkt überall einbinden:** widerspricht ADR 0009 und verteilt die Schalter über alle
  Ziele.
- **Weltlage als `DMat4`:** einfacher, aber doppelt so groß und für die GPU ohnehin umzurechnen.
- **Weltlage als Position, Drehung, Skalierung:** kleiner, aber nicht abgeschlossen unter
  Verkettung, sobald ungleichmäßige Skalierung auf Drehung trifft (Scherung).
