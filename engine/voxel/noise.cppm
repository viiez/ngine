module;

export module ngine.voxel:noise;

import ngine.core;

// Deterministisches Rauschen für den Generator (VOX-05, R3.2, ADR 0039). Gleicher Startwert und
// gleiche Position ergeben auf allen Plattformen und mit allen Compilern dieselben Bits: Gerechnet
// wird nur mit Addition, Subtraktion, Multiplikation und Division in f64 (IEEE 754, korrekt
// gerundet), dazu Vergleiche und Ganzzahlen. Keine Funktionen der Standardbibliothek mit
// plattformabhängiger Genauigkeit (sin, pow, exp) und keine zusammengezogenen Operationen (FMA);
// dafür schaltet CMake sie für ngine.voxel ab.

export namespace ngine::voxel {

// Mischt einen Startwert mit einer Zahl zu einem neuen Startwert (SplitMix64). So bekommt jede
// Rauschschicht und jede Oktave einen eigenen, unabhängigen Startwert.
[[nodiscard]] constexpr u64 mix_seed(u64 seed, u64 salt) noexcept {
    u64 z = seed + 0x9E3779B97F4A7C15ull * (salt + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Abrunden auf eine Ganzzahl, exakt und ohne std::floor.
[[nodiscard]] constexpr i64 floor_to_int(f64 value) noexcept {
    const auto truncated = static_cast<i64>(value);
    return value < static_cast<f64>(truncated) ? truncated - 1 : truncated;
}

// Weicher Übergang von 0 (bei edge0) nach 1 (bei edge1), außerhalb geklemmt.
[[nodiscard]] constexpr f64 smoothstep(f64 edge0, f64 edge1, f64 value) noexcept {
    f64 t = (value - edge0) / (edge1 - edge0);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}

// Simplex-Rauschen in 2D und 3D mit Gradienten aus einem Hash des Gitterpunkts und des
// Startwerts (keine Permutationstabelle). Werte liegen in [-1, 1]; eine Periode ist rund eine
// Längeneinheit der Eingabe.
class Noise {
public:
    constexpr explicit Noise(u64 seed = 0) noexcept : seed_{seed} {}

    [[nodiscard]] constexpr u64 seed() const noexcept {
        return seed_;
    }
    [[nodiscard]] f64 sample(f64 x, f64 y) const noexcept;
    [[nodiscard]] f64 sample(f64 x, f64 y, f64 z) const noexcept;

private:
    u64 seed_;
};

// Mehrere Oktaven übereinander (fraktale Brownsche Bewegung): Jede Oktave hat lacunarity-mal so
// hohe Frequenz und gain-mal so viel Ausschlag wie die vorige und einen eigenen Startwert.
struct Fractal {
    u32 octaves = 4;
    f64 lacunarity = 2.0;
    f64 gain = 0.5;
};

// fBm, auf die Summe der Ausschläge normiert, also in [-1, 1].
[[nodiscard]] f64 fbm(const Noise& noise, f64 x, f64 y, const Fractal& fractal = {}) noexcept;
[[nodiscard]] f64 fbm(const Noise& noise, f64 x, f64 y, f64 z,
                      const Fractal& fractal = {}) noexcept;
// Gratrauschen: (1 - |n|)² pro Oktave, normiert auf [0, 1]. Ergibt scharfe Grate (Felsrücken).
[[nodiscard]] f64 ridged(const Noise& noise, f64 x, f64 y, const Fractal& fractal = {}) noexcept;

} // namespace ngine::voxel
