module;

#include <ngine/profile.hpp>

#include <array>
#include <cmath>
#include <span>
#include <vector>

module ngine.voxel;

import ngine.core;
import ngine.jobs;
import ngine.profile;

namespace ngine::voxel {

namespace {

constexpr f64 max(f64 a, f64 b) noexcept {
    return a > b ? a : b;
}
constexpr f64 min(f64 a, f64 b) noexcept {
    return a < b ? a : b;
}
constexpr f64 lerp(f64 a, f64 b, f64 t) noexcept {
    return a + (b - a) * t;
}

// Meter in Voxel.
f64 voxels(f32 meters) noexcept {
    return static_cast<f64>(meters) / voxel_size;
}

} // namespace

// Was die 2D-Regeln über eine Säule (x, z) wissen.
struct Generator::Column {
    // Höhe der Oberfläche in Voxeln.
    f64 h = 0.0;
    // Länge von (−∂h/∂x, 1, −∂h/∂z): teilt die Höhe über einem Punkt zum Abstand zur Oberfläche.
    f64 norm = 1.0;
    // 0 auf flachem Boden, 1 an steilen Hängen: dort Fels und Überhänge.
    f64 steep = 0.0;
};

// Was die 3D-Regeln an einem Gitterpunkt wissen (alle in [-1, 1] bzw. [0, √2]).
struct Generator::Lattice {
    f64 overhang = 0.0;
    f64 tunnel = 1.0;
    f64 cavern = -1.0;
};

namespace {

using Field = std::array<f64, 3>;

// Trilinear zwischen acht Gitterpunkten c[x + 2y + 4z]; überall in derselben Reihenfolge
// gerechnet, damit fill() und sample() dieselben Bits liefern.
template <typename T>
T trilinear(const std::array<T, 8>& c, f64 tx, f64 ty, f64 tz) noexcept {
    T result;
    const auto mix = [&](auto member) {
        const f64 x00 = lerp(c[0].*member, c[1].*member, tx);
        const f64 x10 = lerp(c[2].*member, c[3].*member, tx);
        const f64 x01 = lerp(c[4].*member, c[5].*member, tx);
        const f64 x11 = lerp(c[6].*member, c[7].*member, tx);
        result.*member = lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
    };
    mix(&T::overhang);
    mix(&T::tunnel);
    mix(&T::cavern);
    return result;
}

} // namespace

Generator::Generator(const GeneratorDesc& desc)
    : desc_{desc}, ground_{voxels(desc.ground)}, hills_{voxels(desc.hills)},
      hill_scale_{max(voxels(desc.hill_scale), 1.0)}, mountains_{voxels(desc.mountains)},
      mountain_scale_{max(voxels(desc.mountain_scale), 1.0)}, warp_{voxels(desc.warp)},
      terrace_{voxels(desc.terrace)}, overhangs_{voxels(desc.overhangs)},
      cave_scale_{max(voxels(desc.cave_scale), 1.0)}, sand_level_{voxels(desc.sand_level)},
      grass_depth_{voxels(desc.grass_depth)}, earth_depth_{voxels(desc.earth_depth)},
      warp_x_{mix_seed(desc.seed, 1)}, warp_z_{mix_seed(desc.seed, 2)},
      hill_noise_{mix_seed(desc.seed, 3)}, mask_noise_{mix_seed(desc.seed, 4)},
      ridge_noise_{mix_seed(desc.seed, 5)}, overhang_noise_{mix_seed(desc.seed, 6)},
      tunnel_a_{mix_seed(desc.seed, 7)}, tunnel_b_{mix_seed(desc.seed, 8)},
      cavern_noise_{mix_seed(desc.seed, 9)} {}

f64 Generator::height(i64 x, i64 z) const noexcept {
    f64 qx = static_cast<f64>(x);
    f64 qz = static_cast<f64>(z);
    // Domain Warping: Die Eingabe wandert um ein langsames Rauschen.
    if (warp_ > 0.0) {
        const f64 scale = hill_scale_ * 1.5;
        const f64 wx = fbm(warp_x_, qx / scale, qz / scale, {.octaves = 3});
        const f64 wz = fbm(warp_z_, qx / scale, qz / scale, {.octaves = 3});
        qx += wx * warp_;
        qz += wz * warp_;
    }
    f64 h = ground_;
    if (hills_ > 0.0) {
        h += hills_ * fbm(hill_noise_, qx / hill_scale_, qz / hill_scale_, {.octaves = 5});
    }
    if (mountains_ > 0.0) {
        // Grobe Maske: Felsrücken nur in einem Teil der Welt, dazwischen Wiesen.
        const f64 m = mask_noise_.sample(qx / mountain_scale_, qz / mountain_scale_);
        const f64 mask = smoothstep(-0.15, 0.45, m);
        if (mask > 0.0) {
            const f64 scale = mountain_scale_ * 0.5;
            const f64 r = ridged(ridge_noise_, qx / scale, qz / scale, {.octaves = 4});
            f64 mh = mountains_ * mask * smoothstep(0.15, 0.85, r);
            // Terrassen: flache Absätze, dazwischen steile Stufen (Felswände).
            const f64 cliffs = min(max(static_cast<f64>(desc_.cliffs), 0.0), 1.0);
            if (cliffs > 0.0 && terrace_ > 0.0) {
                const f64 t = mh / terrace_;
                const auto step = static_cast<f64>(floor_to_int(t));
                const f64 edge = 0.45 * cliffs;
                const f64 stepped = terrace_ * (step + smoothstep(edge, 1.0 - edge, t - step));
                mh = lerp(mh, stepped, cliffs);
            }
            h += mh;
        }
    }
    return h;
}

Generator::Column Generator::column(f64 h, f64 h_x0, f64 h_x1, f64 h_z0, f64 h_z1) const noexcept {
    const f64 gx = (h_x1 - h_x0) * 0.5;
    const f64 gz = (h_z1 - h_z0) * 0.5;
    const f64 norm = std::sqrt(1.0 + gx * gx + gz * gz);
    // 1 - Normale.y: 0 flach, ab rund 40 Grad Fels.
    const f64 steep = smoothstep(0.22, 0.36, 1.0 - 1.0 / norm);
    return {.h = h, .norm = norm, .steep = steep};
}

bool Generator::needs_lattice() const noexcept {
    return overhangs_ > 0.0 || desc_.caves > 0.0f;
}

Generator::Lattice Generator::lattice_point(i64 lx, i64 ly, i64 lz) const noexcept {
    const auto x = static_cast<f64>(lx * lattice);
    const auto y = static_cast<f64>(ly * lattice);
    const auto z = static_cast<f64>(lz * lattice);
    Lattice result;
    if (overhangs_ > 0.0) {
        const f64 s = 24.0;
        result.overhang = fbm(overhang_noise_, x / s, y / s, z / s, {.octaves = 2});
    }
    if (desc_.caves > 0.0f) {
        // Gänge liegen eher waagerecht: y gestaucht.
        const f64 s = cave_scale_;
        const f64 a = tunnel_a_.sample(x / s, y * 1.6 / s, z / s);
        const f64 b = tunnel_b_.sample(x / s, y * 1.6 / s, z / s);
        result.tunnel = std::sqrt(a * a + b * b);
        const f64 c = cave_scale_ * 1.5;
        result.cavern = fbm(cavern_noise_, x / c, y * 1.4 / c, z / c, {.octaves = 2});
    }
    return result;
}

Sample Generator::evaluate(const Column& column, const Lattice& field, i64 y) const noexcept {
    const auto fy = static_cast<f64>(y);
    const f64 depth = column.h - fy;
    const f64 surface = depth / column.norm;
    f64 d = surface;
    if (overhangs_ > 0.0) {
        d += overhangs_ * field.overhang * column.steep;
    }
    const f64 caves = min(static_cast<f64>(desc_.caves), 1.0);
    if (caves > 0.0) {
        // Abstand in Voxeln grob aus dem Gefälle des Rauschens (rund 3,5 pro Längeneinheit).
        const f64 k = cave_scale_ / 3.5;
        const f64 width = 0.06 + 0.12 * caves;
        const f64 tunnel = (field.tunnel - width) * k + max(0.0, 3.0 - depth) * 0.5;
        const f64 threshold = 0.75 - 0.35 * caves;
        const f64 cavern = (threshold - field.cavern) * k * 1.5 + max(0.0, 24.0 - depth) * 0.5;
        // Untere Kante bleibt geschlossen.
        const f64 cave = min(tunnel, cavern) + max(0.0, 4.0 - fy) * 2.0;
        d = min(d, cave);
    }
    const i8 density = quantize(static_cast<f32>(d));
    if (density <= 0) {
        return {density, air};
    }
    Material material = rock;
    if (surface > 0.0 && column.steep <= 0.5 && depth <= earth_depth_) {
        if (column.h < sand_level_) {
            material = sand;
        } else {
            material = depth < grass_depth_ ? grass : earth;
        }
    }
    return {density, material};
}

Sample Generator::sample(i64 x, i64 y, i64 z) const {
    const Column col = column(height(x, z), height(x - 1, z), height(x + 1, z), height(x, z - 1),
                              height(x, z + 1));
    Lattice field;
    if (needs_lattice()) {
        const i64 lx = floor_div(x, lattice);
        const i64 ly = floor_div(y, lattice);
        const i64 lz = floor_div(z, lattice);
        std::array<Lattice, 8> corners;
        for (u32 c = 0; c < 8; ++c) {
            corners[c] = lattice_point(lx + (c & 1u), ly + ((c >> 1) & 1u), lz + ((c >> 2) & 1u));
        }
        const auto step = static_cast<f64>(lattice);
        field = trilinear(corners, static_cast<f64>(x - lx * lattice) / step,
                          static_cast<f64>(y - ly * lattice) / step,
                          static_cast<f64>(z - lz * lattice) / step);
    }
    return evaluate(col, field, y);
}

void Generator::fill(ChunkCoord coord, Chunk& out) const {
    NGINE_PROFILE_ZONE("voxel::Generator::fill");
    verify(coord.level == 0, "voxel::Generator::fill: nur Stufe 0");
    const i64 ox = chunk_origin(coord.x);
    const i64 oy = chunk_origin(coord.y);
    const i64 oz = chunk_origin(coord.z);
    constexpr i64 n = chunk_voxels;

    // Höhen mit einem Rand von einer Säule für das Gefälle, dann die Säulen.
    constexpr i64 hn = n + 2;
    std::vector<f64> heights(static_cast<usize>(hn * hn));
    for (i64 z = 0; z < hn; ++z) {
        for (i64 x = 0; x < hn; ++x) {
            heights[static_cast<usize>(z * hn + x)] = height(ox + x - 1, oz + z - 1);
        }
    }
    const auto h_at = [&](i64 x, i64 z) {
        return heights[static_cast<usize>((z + 1) * hn + x + 1)];
    };
    std::vector<Column> columns(static_cast<usize>(n * n));
    f64 h_min = 1e300;
    f64 h_max = -1e300;
    f64 norm_max = 1.0;
    for (i64 z = 0; z < n; ++z) {
        for (i64 x = 0; x < n; ++x) {
            const Column col =
                column(h_at(x, z), h_at(x - 1, z), h_at(x + 1, z), h_at(x, z - 1), h_at(x, z + 1));
            columns[static_cast<usize>(z * n + x)] = col;
            h_min = min(h_min, col.h);
            h_max = max(h_max, col.h);
            norm_max = max(norm_max, col.norm);
        }
    }

    // Ganz über der Oberfläche: Luft. Der Abstand ist dort höchstens (h_max - y) / norm_max,
    // Überhänge heben ihn um höchstens overhangs_, Höhlen senken ihn nur.
    const f64 margin = static_cast<f64>(max_distance) + 0.1;
    if ((h_max - static_cast<f64>(oy)) / norm_max + overhangs_ <= -margin) {
        out.fill({-127, air});
        return;
    }
    // Ganz darunter und ohne Höhlen: Fels.
    if (desc_.caves <= 0.0f) {
        const f64 depth = h_min - static_cast<f64>(oy + n - 1);
        if (depth / norm_max - overhangs_ >= margin && depth > earth_depth_) {
            out.fill({127, rock});
            return;
        }
    }

    // 3D-Regeln an den Gitterpunkten im Chunk (9³ bei 32 Voxeln und Abstand 4).
    constexpr i64 ln = n / lattice + 1;
    std::vector<Lattice> grid;
    const bool lattice_on = needs_lattice();
    if (lattice_on) {
        grid.resize(static_cast<usize>(ln * ln * ln));
        const i64 lx0 = floor_div(ox, lattice);
        const i64 ly0 = floor_div(oy, lattice);
        const i64 lz0 = floor_div(oz, lattice);
        for (i64 z = 0; z < ln; ++z) {
            for (i64 y = 0; y < ln; ++y) {
                for (i64 x = 0; x < ln; ++x) {
                    grid[static_cast<usize>((z * ln + y) * ln + x)] =
                        lattice_point(lx0 + x, ly0 + y, lz0 + z);
                }
            }
        }
    }

    std::span<Sample> samples = out.expand();
    const auto step = static_cast<f64>(lattice);
    for (i64 z = 0; z < n; ++z) {
        for (i64 y = 0; y < n; ++y) {
            for (i64 x = 0; x < n; ++x) {
                Lattice field;
                if (lattice_on) {
                    const i64 cx = x / lattice;
                    const i64 cy = y / lattice;
                    const i64 cz = z / lattice;
                    std::array<Lattice, 8> corners;
                    for (u32 c = 0; c < 8; ++c) {
                        const i64 gx = cx + (c & 1u);
                        const i64 gy = cy + ((c >> 1) & 1u);
                        const i64 gz = cz + ((c >> 2) & 1u);
                        corners[c] = grid[static_cast<usize>((gz * ln + gy) * ln + gx)];
                    }
                    field = trilinear(corners, static_cast<f64>(x - cx * lattice) / step,
                                      static_cast<f64>(y - cy * lattice) / step,
                                      static_cast<f64>(z - cz * lattice) / step);
                }
                samples[Chunk::index(static_cast<u32>(x), static_cast<u32>(y),
                                     static_cast<u32>(z))] =
                    evaluate(columns[static_cast<usize>(z * n + x)], field, oy + y);
            }
        }
    }
    out.compact();
}

void generate_chunks(jobs::JobSystem& jobs, ChunkStore& store, std::span<const ChunkCoord> coords,
                     const Generator& generator) {
    NGINE_PROFILE_ZONE("voxel::generate_chunks");
    // Anlegen vorab im aufrufenden Thread; danach ändert sich die Tabelle der Chunks nicht mehr.
    std::vector<Chunk*> chunks;
    chunks.reserve(coords.size());
    for (const ChunkCoord coord : coords) {
        verify(coord.level == 0, "voxel::generate_chunks: nur Stufe 0");
        chunks.push_back(&store.get_or_create(coord));
    }
    jobs.parallel_for(coords.size(), 1, [&](usize begin, usize end) {
        for (usize i = begin; i < end; ++i) {
            generator.fill(coords[i], *chunks[i]);
        }
    });
    for (const ChunkCoord coord : coords) {
        store.mark_changed(coord);
    }
}

namespace {

constexpr u64 fnv_offset = 14695981039346656037ull;
constexpr u64 fnv_prime = 1099511628211ull;

constexpr u64 fnv(u64 hash, u8 byte) noexcept {
    return (hash ^ byte) * fnv_prime;
}

u64 hash_chunk(u64 hash, const Chunk& chunk) noexcept {
    for (usize i = 0; i < chunk_sample_count; ++i) {
        const Sample s = chunk.uniform() ? chunk.uniform_value() : chunk.samples()[i];
        hash = fnv(hash, static_cast<u8>(s.density));
        hash = fnv(hash, s.material);
    }
    return hash;
}

} // namespace

u64 checksum(const Chunk& chunk) noexcept {
    return hash_chunk(fnv_offset, chunk);
}

u64 checksum(const ChunkStore& store) {
    u64 hash = fnv_offset;
    for (const ChunkCoord coord : store.coords()) {
        for (const i32 value : {coord.x, coord.y, coord.z}) {
            const auto bits = static_cast<u32>(value);
            for (u32 shift = 0; shift < 32; shift += 8) {
                hash = fnv(hash, static_cast<u8>(bits >> shift));
            }
        }
        hash = hash_chunk(hash, *store.find(coord));
    }
    return hash;
}

} // namespace ngine::voxel
