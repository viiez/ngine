module;

#include <array>

module ngine.voxel;

import ngine.core;

namespace ngine::voxel {

namespace {

// Hash eines Gitterpunkts mit dem Startwert; nur Ganzzahlen, also überall gleich.
constexpr u64 hash(u64 seed, i64 i, i64 j, i64 k = 0) noexcept {
    u64 h = seed ^ (static_cast<u64>(i) * 0x9E3779B97F4A7C15ull) ^
            (static_cast<u64>(j) * 0xC2B2AE3D27D4EB4Full) ^
            (static_cast<u64>(k) * 0x165667B19E3779F9ull);
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return h;
}

// 16 Richtungen im Abstand von 22,5 Grad, als Zahlen ausgeschrieben statt mit cos und sin
// gerechnet, damit keine Plattform anders rundet.
constexpr std::array<std::array<f64, 2>, 16> gradients2{{
    {1.0, 0.0},
    {0.9238795325112867, 0.3826834323650898},
    {0.7071067811865476, 0.7071067811865476},
    {0.3826834323650898, 0.9238795325112867},
    {0.0, 1.0},
    {-0.3826834323650898, 0.9238795325112867},
    {-0.7071067811865476, 0.7071067811865476},
    {-0.9238795325112867, 0.3826834323650898},
    {-1.0, 0.0},
    {-0.9238795325112867, -0.3826834323650898},
    {-0.7071067811865476, -0.7071067811865476},
    {-0.3826834323650898, -0.9238795325112867},
    {0.0, -1.0},
    {0.3826834323650898, -0.9238795325112867},
    {0.7071067811865476, -0.7071067811865476},
    {0.9238795325112867, -0.3826834323650898},
}};

// Die zwölf Kantenmitten des Würfels, vier davon doppelt (16 Einträge für eine Maske).
constexpr std::array<std::array<f64, 3>, 16> gradients3{{
    {1, 1, 0},
    {-1, 1, 0},
    {1, -1, 0},
    {-1, -1, 0},
    {1, 0, 1},
    {-1, 0, 1},
    {1, 0, -1},
    {-1, 0, -1},
    {0, 1, 1},
    {0, -1, 1},
    {0, 1, -1},
    {0, -1, -1},
    {1, 1, 0},
    {-1, 1, 0},
    {0, -1, 1},
    {0, -1, -1},
}};

// Schiefe des Simplex-Gitters: F = (√(n+1) - 1) / n, G = (1 - 1/√(n+1)) / n, ausgeschrieben.
constexpr f64 skew2 = 0.36602540378443864676;
constexpr f64 unskew2 = 0.21132486540518711775;
constexpr f64 skew3 = 1.0 / 3.0;
constexpr f64 unskew3 = 1.0 / 6.0;

f64 corner2(u64 seed, i64 i, i64 j, f64 x, f64 y) noexcept {
    f64 t = 0.5 - x * x - y * y;
    if (t <= 0.0) {
        return 0.0;
    }
    const auto& g = gradients2[hash(seed, i, j) & 15u];
    t *= t;
    return t * t * (g[0] * x + g[1] * y);
}

f64 corner3(u64 seed, i64 i, i64 j, i64 k, f64 x, f64 y, f64 z) noexcept {
    f64 t = 0.6 - x * x - y * y - z * z;
    if (t <= 0.0) {
        return 0.0;
    }
    const auto& g = gradients3[hash(seed, i, j, k) & 15u];
    t *= t;
    return t * t * (g[0] * x + g[1] * y + g[2] * z);
}

f64 clamp_unit(f64 value) noexcept {
    return value < -1.0 ? -1.0 : (value > 1.0 ? 1.0 : value);
}

} // namespace

f64 Noise::sample(f64 x, f64 y) const noexcept {
    // Zelle im schiefen Gitter, dann die drei Ecken des Dreiecks, in dem der Punkt liegt.
    const f64 s = (x + y) * skew2;
    const i64 i = floor_to_int(x + s);
    const i64 j = floor_to_int(y + s);
    const f64 t = static_cast<f64>(i + j) * unskew2;
    const f64 x0 = x - (static_cast<f64>(i) - t);
    const f64 y0 = y - (static_cast<f64>(j) - t);
    const i64 i1 = x0 > y0 ? 1 : 0;
    const i64 j1 = 1 - i1;
    const f64 x1 = x0 - static_cast<f64>(i1) + unskew2;
    const f64 y1 = y0 - static_cast<f64>(j1) + unskew2;
    const f64 x2 = x0 - 1.0 + 2.0 * unskew2;
    const f64 y2 = y0 - 1.0 + 2.0 * unskew2;
    const f64 n = corner2(seed_, i, j, x0, y0) + corner2(seed_, i + i1, j + j1, x1, y1) +
                  corner2(seed_, i + 1, j + 1, x2, y2);
    return clamp_unit(70.0 * n);
}

f64 Noise::sample(f64 x, f64 y, f64 z) const noexcept {
    const f64 s = (x + y + z) * skew3;
    const i64 i = floor_to_int(x + s);
    const i64 j = floor_to_int(y + s);
    const i64 k = floor_to_int(z + s);
    const f64 t = static_cast<f64>(i + j + k) * unskew3;
    const f64 x0 = x - (static_cast<f64>(i) - t);
    const f64 y0 = y - (static_cast<f64>(j) - t);
    const f64 z0 = z - (static_cast<f64>(k) - t);
    // Welches der sechs Tetraeder des Würfels: nach der Reihenfolge der Anteile.
    i64 i1 = 0, j1 = 0, k1 = 0, i2 = 0, j2 = 0, k2 = 0;
    if (x0 >= y0) {
        if (y0 >= z0) {
            i1 = 1;
            i2 = 1;
            j2 = 1;
        } else if (x0 >= z0) {
            i1 = 1;
            i2 = 1;
            k2 = 1;
        } else {
            k1 = 1;
            i2 = 1;
            k2 = 1;
        }
    } else {
        if (y0 < z0) {
            k1 = 1;
            j2 = 1;
            k2 = 1;
        } else if (x0 < z0) {
            j1 = 1;
            j2 = 1;
            k2 = 1;
        } else {
            j1 = 1;
            i2 = 1;
            j2 = 1;
        }
    }
    const f64 x1 = x0 - static_cast<f64>(i1) + unskew3;
    const f64 y1 = y0 - static_cast<f64>(j1) + unskew3;
    const f64 z1 = z0 - static_cast<f64>(k1) + unskew3;
    const f64 x2 = x0 - static_cast<f64>(i2) + 2.0 * unskew3;
    const f64 y2 = y0 - static_cast<f64>(j2) + 2.0 * unskew3;
    const f64 z2 = z0 - static_cast<f64>(k2) + 2.0 * unskew3;
    const f64 x3 = x0 - 1.0 + 3.0 * unskew3;
    const f64 y3 = y0 - 1.0 + 3.0 * unskew3;
    const f64 z3 = z0 - 1.0 + 3.0 * unskew3;
    const f64 n = corner3(seed_, i, j, k, x0, y0, z0) +
                  corner3(seed_, i + i1, j + j1, k + k1, x1, y1, z1) +
                  corner3(seed_, i + i2, j + j2, k + k2, x2, y2, z2) +
                  corner3(seed_, i + 1, j + 1, k + 1, x3, y3, z3);
    return clamp_unit(32.0 * n);
}

f64 fbm(const Noise& noise, f64 x, f64 y, const Fractal& fractal) noexcept {
    f64 sum = 0.0;
    f64 amplitude = 1.0;
    f64 total = 0.0;
    f64 frequency = 1.0;
    for (u32 octave = 0; octave < fractal.octaves; ++octave) {
        const Noise layer{mix_seed(noise.seed(), octave)};
        sum += amplitude * layer.sample(x * frequency, y * frequency);
        total += amplitude;
        amplitude *= fractal.gain;
        frequency *= fractal.lacunarity;
    }
    return total > 0.0 ? sum / total : 0.0;
}

f64 fbm(const Noise& noise, f64 x, f64 y, f64 z, const Fractal& fractal) noexcept {
    f64 sum = 0.0;
    f64 amplitude = 1.0;
    f64 total = 0.0;
    f64 frequency = 1.0;
    for (u32 octave = 0; octave < fractal.octaves; ++octave) {
        const Noise layer{mix_seed(noise.seed(), octave)};
        sum += amplitude * layer.sample(x * frequency, y * frequency, z * frequency);
        total += amplitude;
        amplitude *= fractal.gain;
        frequency *= fractal.lacunarity;
    }
    return total > 0.0 ? sum / total : 0.0;
}

f64 ridged(const Noise& noise, f64 x, f64 y, const Fractal& fractal) noexcept {
    f64 sum = 0.0;
    f64 amplitude = 1.0;
    f64 total = 0.0;
    f64 frequency = 1.0;
    for (u32 octave = 0; octave < fractal.octaves; ++octave) {
        const Noise layer{mix_seed(noise.seed(), octave)};
        const f64 n = layer.sample(x * frequency, y * frequency);
        const f64 ridge = 1.0 - (n < 0.0 ? -n : n);
        sum += amplitude * ridge * ridge;
        total += amplitude;
        amplitude *= fractal.gain;
        frequency *= fractal.lacunarity;
    }
    return total > 0.0 ? sum / total : 0.0;
}

} // namespace ngine::voxel
