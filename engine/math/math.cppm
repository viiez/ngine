// Mathematik für Spielwelt und Rendering (CORE-10). Grundlage ist GLM (ADR 0025); dieses Modul
// legt fest, welche Typen und Funktionen die Engine nutzt, und ergänzt, was GLM nicht hat.
//
// Konventionen:
//   - Rechtshändig, y zeigt nach oben, die Kamera blickt entlang -z.
//   - Spaltenvektoren: p' = M * p, Matrizen von rechts nach links (P * V * M).
//   - Matrizen liegen spaltenweise im Speicher (m[spalte][zeile]), wie in Vulkan und GLSL.
//   - Winkel im Bogenmaß, Tiefe im Clip-Raum von 0 bis 1 (Vulkan).
//
//   :types      Vektoren, Matrizen, Quaternionen und Grundfunktionen aus GLM
//   :transform  Drehungen, Matrizen, Projektionen, Vergleiche mit Toleranz
//   :affine     Affine: Drehung und Skalierung in 32 Bit, Verschiebung in 64 Bit (ADR 0012)
export module ngine.math;

export import :types;
export import :transform;
export import :affine;
