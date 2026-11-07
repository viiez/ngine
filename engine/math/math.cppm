// Mathematik für Spielwelt und Rendering (CORE-10, ADR 0025). Eigene, kleine Typen ohne
// Fremdbibliothek; gerechnet wird skalar, der Compiler vektorisiert, wo es lohnt. SIMD kommt erst
// dort, wo eine Messung es verlangt.
//
// Konventionen:
//   - Rechtshändig, y zeigt nach oben, die Kamera blickt entlang -z.
//   - Spaltenvektoren: p' = M * p, Matrizen von rechts nach links (P * V * M).
//   - Matrizen liegen spaltenweise im Speicher (m[spalte][zeile]), wie in Vulkan und GLSL.
//   - Winkel im Bogenmaß, Tiefe im Clip-Raum von 0 bis 1 (Vulkan).
//
//   :scalar      Konstanten, Winkel, Vergleich mit Toleranz
//   :vector      Vec2, Vec3, Vec4, IVec2, IVec3 und DVec3 (64 Bit) mit Funktionen
//   :matrix      Mat3, Mat4, DMat3, Umkehrmatrix und Determinante
//   :quaternion  Quat für Drehungen, slerp, look_rotation
//   :transform   compose, look_at, Projektionen
//   :affine      Affine: Drehung und Skalierung in 32 Bit, Verschiebung in 64 Bit (ADR 0012),
//                zerlegen in Verschiebung, Drehung und Skalierung
//   :ray         Strahlen, Abstände und Schnitte für Treffertests (64 Bit)
export module ngine.math;

export import :scalar;
export import :vector;
export import :matrix;
export import :quaternion;
export import :transform;
export import :affine;
export import :ray;
