module;

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <ngine/profile.hpp>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

module ngine.voxel;

import ngine.core;
import ngine.math;
import ngine.meshlet;
import ngine.profile;

namespace ngine::voxel {

using math::length;
using math::normalize;

namespace {

constexpr u32 no_vertex = std::numeric_limits<u32>::max();

// Die zwölf Kanten eines Voxels als Paare von Ecken. Ecke i liegt bei (i & 1, i >> 1 & 1, i >> 2 &
// 1).
constexpr std::array<std::array<u32, 2>, 12> cube_edges{{
    {0, 1},
    {2, 3},
    {4, 5},
    {6, 7}, // entlang x
    {0, 2},
    {1, 3},
    {4, 6},
    {5, 7}, // entlang y
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7}, // entlang z
}};

Vec3 corner_offset(u32 corner) {
    return {static_cast<f32>(corner & 1u), static_cast<f32>((corner >> 1) & 1u),
            static_cast<f32>((corner >> 2) & 1u)};
}

// Platz eines Materials in der Palette des Netzes; neue Materialien bekommen den nächsten freien
// Platz. Ist die Palette voll, zählt das Material zu Platz 0 (ADR 0040).
u32 palette_slot(Mesh& mesh, Material material) {
    for (u32 slot = 0; slot < mesh.palette_count; ++slot) {
        if (mesh.palette[slot] == material) {
            return slot;
        }
    }
    if (mesh.palette_count == palette_size) {
        return 0;
    }
    mesh.palette[mesh.palette_count] = material;
    return mesh.palette_count++;
}

// Teilt das fertige Netz in Meshlets (R3.5) und sortiert dafür seine Dreiecke um.
void build_meshlets(Mesh& mesh) {
    mesh.meshlets =
        meshlet::build(meshlet::positions_of(std::span<const Vertex>{mesh.vertices}), mesh.indices);
    mesh.local = meshlet::local_data(mesh.meshlets, mesh.indices);
}

// Gewichte auf 8 Bit pro Platz mit der Summe 255; der Rest der Rundung geht an den größten Platz.
std::pair<u32, u32> pack_weights(const std::array<f32, palette_size>& weights) {
    f32 total = 0.0f;
    u32 largest = 0;
    for (u32 slot = 0; slot < palette_size; ++slot) {
        total += weights[slot];
        if (weights[slot] > weights[largest]) {
            largest = slot;
        }
    }
    std::array<u32, palette_size> bytes{};
    u32 sum = 0;
    for (u32 slot = 0; slot < palette_size; ++slot) {
        bytes[slot] = static_cast<u32>(weights[slot] / total * 255.0f + 0.5f);
        sum += bytes[slot];
    }
    // Der größte Platz hat mindestens ein Achtel von 255, der Ausgleich (wenige Einheiten) passt.
    bytes[largest] =
        static_cast<u32>(static_cast<i32>(bytes[largest]) + 255 - static_cast<i32>(sum));
    u32 low = 0;
    u32 high = 0;
    for (u32 slot = 0; slot < 4; ++slot) {
        low |= bytes[slot] << (slot * 8);
        high |= bytes[slot + 4] << (slot * 8);
    }
    return {low, high};
}

} // namespace

void Mesher::build(const Grid& grid, Mesh& out) {
    const Extent3 size = grid.size();
    build_range(grid, out, 0, std::max({size.x, size.y, size.z}) - 1, 0, 0.0f);
}

void Mesher::build_chunk(const Grid& padded, Mesh& out) {
    verify(padded.size() == Extent3{padded_samples, padded_samples, padded_samples},
           "Mesher::build_chunk: Gitter muss padded_samples³ groß sein");
    // Zellen ab einer vor dem Chunk (Eckpunkte für die Vierecke an seinen unteren Kanten), Kanten
    // ab dem ersten eigenen Sample.
    constexpr auto first_cell = static_cast<u32>(border_before - 1);
    constexpr auto end_cell = static_cast<u32>(border_before + chunk_voxels);
    constexpr auto first_edge = static_cast<u32>(border_before);
    build_range(padded, out, first_cell, end_cell, first_edge, static_cast<f32>(border_before));
}

void Mesher::build_range(const Grid& grid, Mesh& out, u32 cell_min, u32 cell_max, u32 edge_min,
                         f32 offset) {
    NGINE_PROFILE_ZONE("voxel::Mesher::build");
    out.clear();

    const Extent3 samples = grid.size();
    const Extent3 cells{samples.x - 1, samples.y - 1, samples.z - 1};
    cell_vertex_.assign(cells.count(), no_vertex);
    const usize cell_stride_y = cells.x;
    const usize cell_stride_z = usize{cells.x} * cells.y;

    // Abstände der acht Ecken eines Voxels im Sample-Array.
    const std::span<const Sample> data = grid.samples();
    const usize stride_y = samples.x;
    const usize stride_z = usize{samples.x} * samples.y;
    std::array<usize, 8> corner_offsets{};
    for (u32 c = 0; c < 8; ++c) {
        corner_offsets[c] = (c & 1u) + ((c >> 1) & 1u) * stride_y + ((c >> 2) & 1u) * stride_z;
    }

    auto emit_quad = [&](u32 v0, u32 v1, u32 v2, u32 v3, bool flip) {
        if (flip) {
            out.indices.insert(out.indices.end(), {v0, v3, v2, v0, v2, v1});
        } else {
            out.indices.insert(out.indices.end(), {v0, v1, v2, v0, v2, v3});
        }
    };

    // Ein Durchlauf über alle Voxel. Ein Voxel, das die Oberfläche schneidet, bekommt einen
    // Eckpunkt: das Mittel der Schnittpunkte auf seinen Kanten. Danach prüft es die drei Kanten an
    // seiner unteren Ecke; wechselt dort das Vorzeichen, entsteht ein Viereck aus den vier Voxeln
    // um die Kante. Die drei anderen liegen in Richtung kleinerer Koordinaten und haben ihren
    // Eckpunkt also schon. Kanten am Rand haben keine vier Voxel und bleiben offen (Chunk-Grenze,
    // ADR 0021).
    const u32 end_x = std::min(cell_max, cells.x);
    const u32 end_y = std::min(cell_max, cells.y);
    const u32 end_z = std::min(cell_max, cells.z);
    for (u32 z = cell_min; z < end_z; ++z) {
        for (u32 y = cell_min; y < end_y; ++y) {
            usize base = z * stride_z + y * stride_y + cell_min;
            usize cell = z * cell_stride_z + y * cell_stride_y + cell_min;
            for (u32 x = cell_min; x < end_x; ++x, ++base, ++cell) {
                // Schneller Test nur mit den Vorzeichen der 8-Bit-Werte.
                u32 inside = 0;
                for (u32 c = 0; c < 8; ++c) {
                    inside |= (data[base + corner_offsets[c]].density > 0 ? 1u : 0u) << c;
                }
                if (inside == 0 || inside == 0xFF) {
                    continue;
                }

                std::array<f32, 8> d{};
                for (u32 c = 0; c < 8; ++c) {
                    d[c] = dequantize(data[base + corner_offsets[c]].density);
                }
                Vec3 sum{};
                u32 crossings = 0;
                for (const auto& [a, b] : cube_edges) {
                    if (((inside >> a) & 1u) == ((inside >> b) & 1u)) {
                        continue;
                    }
                    const f32 t = d[a] / (d[a] - d[b]);
                    sum = sum + corner_offset(a) + (corner_offset(b) - corner_offset(a)) * t;
                    ++crossings;
                }
                const Vec3 position =
                    Vec3{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)} +
                    sum * (1.0f / static_cast<f32>(crossings));
                const Vec3 output = position - Vec3{offset, offset, offset};

                // Gewichte der Materialien aus den festen Ecken: je näher an der Oberfläche,
                // desto mehr. Tiefere Ecken gehören oft schon zum Material darunter (Erde unter
                // Gras) und zählen kaum. Benachbarte Zellen teilen sich Ecken, also verlaufen die
                // Gewichte stetig (ADR 0040).
                std::array<f32, palette_size> weights{};
                for (u32 c = 0; c < 8; ++c) {
                    if (d[c] <= 0.0f) {
                        continue;
                    }
                    const f32 near = d[c] + 0.25f;
                    weights[palette_slot(out, data[base + corner_offsets[c]].material)] +=
                        1.0f / (near * near);
                }
                const auto [weights_low, weights_high] = pack_weights(weights);

                // Die Dichte steigt nach innen; die Normale zeigt nach außen.
                const Vec3 normal = normalize(grid.gradient_at(position) * -1.0f);
                const auto vertex = static_cast<u32>(out.vertices.size());
                cell_vertex_[cell] = vertex;
                out.vertices.push_back({output.x, output.y, output.z, weights_low, normal.x,
                                        normal.y, normal.z, weights_high});

                // Vierecke um die Kanten an Ecke 0. Reihenfolge der Voxel so, dass das Viereck
                // gegen den Uhrzeigersinn liegt, wenn fester Stoff am Anfang der Kante liegt. Die
                // drei Nachbarzellen müssen im Bereich liegen, der Anfang der Kante im eigenen.
                const bool solid = (inside & 1u) != 0;
                const bool own_x = x >= edge_min;
                const bool own_y = y >= edge_min;
                const bool own_z = z >= edge_min;
                const bool below_x = x > cell_min;
                const bool below_y = y > cell_min;
                const bool below_z = z > cell_min;
                if (own_x && below_y && below_z &&
                    solid != ((inside & 2u) != 0)) { // Kante entlang x
                    emit_quad(cell_vertex_[cell - cell_stride_y - cell_stride_z],
                              cell_vertex_[cell - cell_stride_z], vertex,
                              cell_vertex_[cell - cell_stride_y], !solid);
                }
                if (own_y && below_x && below_z &&
                    solid != ((inside & 4u) != 0)) { // Kante entlang y
                    emit_quad(cell_vertex_[cell - 1 - cell_stride_z], cell_vertex_[cell - 1],
                              vertex, cell_vertex_[cell - cell_stride_z], !solid);
                }
                if (own_z && below_x && below_y &&
                    solid != ((inside & 16u) != 0)) { // Kante entlang z
                    emit_quad(cell_vertex_[cell - 1 - cell_stride_y],
                              cell_vertex_[cell - cell_stride_y], vertex, cell_vertex_[cell - 1],
                              !solid);
                }
            }
        }
    }
    build_meshlets(out);
}

// --- LOD (R3.3, ADR 0041) -------------------------------------------------------------------

namespace {

// Wert der Dichte in Voxeln (Stufe 0) samt Material.
struct FieldValue {
    f32 d = 0.0f;
    Material m = air;
};

// Werte einer Zelle und ihrer Nachbarn, 4 × 4 × 4, in Einheiten der Zellgröße; die Zelle selbst
// liegt bei den Indizes 1 und 2. Daraus entstehen Lage, Normale und Gewichte ihres Eckpunkts,
// gleich, welches Blatt die Zelle vermascht.
struct CellBlock {
    std::array<f32, 64> d{};
    std::array<Material, 64> m{};

    [[nodiscard]] static constexpr u32 index(u32 x, u32 y, u32 z) noexcept {
        return (z * 4 + y) * 4 + x;
    }
    // Trilinear wie Grid::distance_at() auf einem Gitter mit 4 Samples pro Achse.
    [[nodiscard]] f32 distance_at(Vec3 p) const noexcept {
        const auto split = [](f32 v) {
            const f32 c = std::clamp(v, 0.0f, 3.0f);
            const auto i0 = std::min(static_cast<u32>(c), 2u);
            return std::pair{i0, c - static_cast<f32>(i0)};
        };
        const auto [x0, tx] = split(p.x);
        const auto [y0, ty] = split(p.y);
        const auto [z0, tz] = split(p.z);
        const auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
        const auto at = [&](u32 x, u32 y, u32 z) { return d[index(x, y, z)]; };
        const f32 c00 = lerp(at(x0, y0, z0), at(x0 + 1, y0, z0), tx);
        const f32 c10 = lerp(at(x0, y0 + 1, z0), at(x0 + 1, y0 + 1, z0), tx);
        const f32 c01 = lerp(at(x0, y0, z0 + 1), at(x0 + 1, y0, z0 + 1), tx);
        const f32 c11 = lerp(at(x0, y0 + 1, z0 + 1), at(x0 + 1, y0 + 1, z0 + 1), tx);
        return lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
    }
};

// Eckpunkt einer Zelle aus ihrem Block; nullopt, wenn die Oberfläche die Zelle nicht schneidet.
// Gleiche Rechnung wie Mesher::build_range(), damit die Netze gleich aussehen.
struct CellVertex {
    Vec3 local;
    Vec3 normal;
    std::array<f32, 8> d{};
    std::array<Material, 8> m{};
};

std::optional<CellVertex> cell_vertex(const CellBlock& block) noexcept {
    CellVertex v;
    u32 inside = 0;
    for (u32 c = 0; c < 8; ++c) {
        const u32 i = CellBlock::index(1 + (c & 1u), 1 + ((c >> 1) & 1u), 1 + ((c >> 2) & 1u));
        v.d[c] = block.d[i];
        v.m[c] = block.m[i];
        inside |= (v.d[c] > 0.0f ? 1u : 0u) << c;
    }
    if (inside == 0 || inside == 0xFF) {
        return std::nullopt;
    }
    Vec3 sum{};
    u32 crossings = 0;
    for (const auto& [a, b] : cube_edges) {
        if (((inside >> a) & 1u) == ((inside >> b) & 1u)) {
            continue;
        }
        const f32 t = v.d[a] / (v.d[a] - v.d[b]);
        sum = sum + corner_offset(a) + (corner_offset(b) - corner_offset(a)) * t;
        ++crossings;
    }
    v.local = sum * (1.0f / static_cast<f32>(crossings));
    const Vec3 p = v.local + Vec3{1.0f, 1.0f, 1.0f};
    const Vec3 gradient{
        (block.distance_at(p + Vec3{1, 0, 0}) - block.distance_at(p - Vec3{1, 0, 0})) * 0.5f,
        (block.distance_at(p + Vec3{0, 1, 0}) - block.distance_at(p - Vec3{0, 1, 0})) * 0.5f,
        (block.distance_at(p + Vec3{0, 0, 1}) - block.distance_at(p - Vec3{0, 0, 1})) * 0.5f,
    };
    v.normal = normalize(gradient * -1.0f);
    return v;
}

// Eine Zelle irgendeines Blatts (oder eine gedachte in einem Bereich ohne Blatt): Ecke in Voxeln
// und Stufe.
struct CellRef {
    i64 x = 0, y = 0, z = 0;
    u8 level = 0;
    bool virtual_cell = false;

    friend bool operator==(const CellRef&, const CellRef&) noexcept = default;
};

struct CellRefHash {
    [[nodiscard]] usize operator()(const CellRef& c) const noexcept {
        u64 h = static_cast<u64>(c.x) * 0x9E3779B97F4A7C15ull;
        h ^= (static_cast<u64>(c.y) + (h << 6) + (h >> 2)) * 0xBF58476D1CE4E5B9ull;
        h ^= (static_cast<u64>(c.z) + (h << 6) + (h >> 2)) * 0x94D049BB133111EBull;
        h ^= c.level * 0x100u + (c.virtual_cell ? 1u : 0u);
        return static_cast<usize>(h ^ (h >> 31));
    }
};

// Gitter des eigenen Blatts in eigener Auflösung mit zwei Samples Rand: Indizes -2 bis 34.
constexpr i32 pad = 2;
constexpr u32 padded_lod = chunk_voxels + 2 * pad + 1;
// Zellen -1 bis 31.
constexpr u32 lod_cells = chunk_voxels + 1;
constexpr u32 vertex_pending = no_vertex - 1;

} // namespace

struct LodMesher::Impl {
    struct Leaf {
        const Chunk* chunk = nullptr;
        ChunkCoord coord{};
        bool present = false;
    };

    const LodLayout* layout = nullptr;
    const ChunkSource* store = nullptr;
    ChunkCoord self{};
    i64 step = 1;
    std::array<i64, 3> origin{};
    // Raster aus 4³ halben Chunks um das Blatt, verdoppelte Koordinaten.
    std::array<i64, 3> base2{};
    i64 block2 = 0;
    std::array<Leaf, 64> blocks{};
    std::array<i8, 729> touching{};
    // Alle berührenden Blätter (sofern vorhanden) haben die eigene Stufe.
    std::array<bool, 729> same_level{};
    i64 half = 16;
    FieldValue outside;

    std::vector<f32> pd = std::vector<f32>(usize{padded_lod} * padded_lod * padded_lod);
    std::vector<Material> pm = std::vector<Material>(usize{padded_lod} * padded_lod * padded_lod);
    std::vector<u32> cells = std::vector<u32>(usize{lod_cells} * lod_cells * lod_cells);
    std::unordered_map<CellRef, u32, CellRefHash> foreign;
    Mesh* out = nullptr;

    static usize pindex(i32 x, i32 y, i32 z) noexcept {
        return (static_cast<usize>(z + pad) * padded_lod + static_cast<usize>(y + pad)) *
                   padded_lod +
               static_cast<usize>(x + pad);
    }
    static usize cindex(i32 x, i32 y, i32 z) noexcept {
        return (static_cast<usize>(z + 1) * lod_cells + static_cast<usize>(y + 1)) * lod_cells +
               static_cast<usize>(x + 1);
    }

    Leaf make_leaf(std::optional<ChunkCoord> coord) const noexcept {
        if (!coord) {
            return {};
        }
        return {store->find(*coord), *coord, true};
    }

    // Blatt am Punkt (verdoppelte Koordinaten, ungerade). Nahe Punkte über das Raster, sonst über
    // den Baum.
    Leaf leaf_at2(i64 x2, i64 y2, i64 z2) const noexcept {
        const i64 bx = floor_div(x2 - base2[0], block2);
        const i64 by = floor_div(y2 - base2[1], block2);
        const i64 bz = floor_div(z2 - base2[2], block2);
        if (bx >= 0 && bx < 4 && by >= 0 && by < 4 && bz >= 0 && bz < 4) {
            return blocks[static_cast<usize>((bz * 4 + by) * 4 + bx)];
        }
        return make_leaf(layout->leaf_at2(x2, y2, z2));
    }

    // Gespeicherter Wert eines Blatts an einem Punkt seines Rasters.
    FieldValue stored(const Leaf& leaf, i64 x, i64 y, i64 z) const noexcept {
        if (leaf.chunk == nullptr) {
            return outside;
        }
        const u8 l = leaf.coord.level;
        const auto lx = static_cast<u32>((x - chunk_origin(leaf.coord.x, l)) >> l);
        const auto ly = static_cast<u32>((y - chunk_origin(leaf.coord.y, l)) >> l);
        const auto lz = static_cast<u32>((z - chunk_origin(leaf.coord.z, l)) >> l);
        const Sample s = leaf.chunk->sample(lx, ly, lz);
        return {dequantize(s.density) * static_cast<f32>(i64{1} << l), s.material};
    }

    // Wert am Punkt aus dem Blatt, dem er gehört; liegt er nicht auf dessen Raster, interpoliert.
    FieldValue value(i64 x, i64 y, i64 z) const noexcept {
        const Leaf owner = leaf_at2(2 * x + 1, 2 * y + 1, 2 * z + 1);
        if (!owner.present) {
            return outside;
        }
        const i64 mask = (i64{1} << owner.coord.level) - 1;
        if (((x | y | z) & mask) == 0) {
            return stored(owner, x, y, z);
        }
        return interpolate(x, y, z, owner.coord.level);
    }

    // Multilinear zwischen den Punkten des Rasters 2^level um den Punkt.
    FieldValue interpolate(i64 x, i64 y, i64 z, u8 level) const noexcept {
        const i64 s = i64{1} << level;
        const auto axis = [&](i64 v) {
            const i64 lo = floor_div(v, s) * s;
            return std::pair{lo, static_cast<f32>(v - lo) / static_cast<f32>(s)};
        };
        const auto [x0, tx] = axis(x);
        const auto [y0, ty] = axis(y);
        const auto [z0, tz] = axis(z);
        // Nur Ecken entlang der Achsen, auf denen der Punkt nicht auf dem Raster liegt; die anderen
        // sind Kopien.
        const u32 moving = (tx > 0.0f ? 1u : 0u) | (ty > 0.0f ? 2u : 0u) | (tz > 0.0f ? 4u : 0u);
        std::array<FieldValue, 8> c;
        for (u32 i = 0; i < 8; ++i) {
            const u32 base = i & moving;
            if (base != i) {
                c[i] = c[base];
                continue;
            }
            c[i] = value((i & 1u) != 0 ? x0 + s : x0, (i & 2u) != 0 ? y0 + s : y0,
                         (i & 4u) != 0 ? z0 + s : z0);
        }
        const auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
        const f32 x00 = lerp(c[0].d, c[1].d, tx);
        const f32 x10 = lerp(c[2].d, c[3].d, tx);
        const f32 x01 = lerp(c[4].d, c[5].d, tx);
        const f32 x11 = lerp(c[6].d, c[7].d, tx);
        FieldValue result{lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz), air};
        // Material der Ecke, die am tiefsten im Inneren liegt.
        FieldValue best = c[0];
        for (const FieldValue& v : c) {
            if (v.d > best.d) {
                best = v;
            }
        }
        result.m = best.m;
        return result;
    }

    // Das Feld, aus dem alle Blätter ihre Netze bauen (ADR 0041): Ein Punkt auf einer Grenze
    // zwischen Stufen gehört zum Raster der gröbsten Stufe, die ihn berührt; liegt er nicht
    // darauf, wird er aus ihren Punkten interpoliert. So sehen beide Seiten dieselben Werte, und
    // auf der Grenzfläche verläuft das Feld linear wie auf der groben Seite.
    FieldValue field(i64 x, i64 y, i64 z) const noexcept {
        i32 level = -1;
        // Nahe Punkte: Welche Stufen den Punkt berühren, hängt nur davon ab, ob er im Inneren
        // eines halben Chunks des Rasters liegt oder auf einer Grenze zwischen zweien.
        const auto category = [&](i64 v, usize axis) -> i32 {
            const i64 rel = v - (origin[axis] - half);
            const i64 q = floor_div(rel, half);
            const i64 r = rel - q * half;
            if (r != 0) {
                return q >= 0 && q < 4 ? static_cast<i32>(2 * q + 1) : -1;
            }
            return q >= 1 && q <= 3 ? static_cast<i32>(2 * q) : -1;
        };
        const i32 cx = category(x, 0);
        const i32 cy = category(y, 1);
        const i32 cz = category(z, 2);
        if (cx >= 0 && cy >= 0 && cz >= 0) {
            level = touching[static_cast<usize>((cz * 9 + cy) * 9 + cx)];
        } else {
            level = -1;
            for (u32 i = 0; i < 8; ++i) {
                const Leaf leaf = leaf_at2(2 * x + ((i & 1u) != 0 ? 1 : -1),
                                           2 * y + ((i & 2u) != 0 ? 1 : -1),
                                           2 * z + ((i & 4u) != 0 ? 1 : -1));
                if (leaf.present) {
                    level = std::max<i32>(level, leaf.coord.level);
                }
            }
        }
        if (level < 0) {
            return outside;
        }
        const i64 mask = (i64{1} << level) - 1;
        if (((x | y | z) & mask) == 0) {
            return value(x, y, z);
        }
        return interpolate(x, y, z, static_cast<u8>(level));
    }

    // Größte Stufe der Blätter, die einen Punkt berühren, nach Lage im Raster der halben Chunks:
    // pro Achse 0, 2, 4, 6, 8 auf einer Grenze, 1, 3, 5, 7 im Inneren (9³ Fälle). -1: kein Blatt.
    void classify() noexcept {
        for (i32 cz = 0; cz < 9; ++cz) {
            for (i32 cy = 0; cy < 9; ++cy) {
                for (i32 cx = 0; cx < 9; ++cx) {
                    const auto range = [](i32 c) {
                        return (c % 2) != 0 ? std::pair{c / 2, c / 2}
                                            : std::pair{std::max(c / 2 - 1, 0), std::min(c / 2, 3)};
                    };
                    const auto [x0, x1] = range(cx);
                    const auto [y0, y1] = range(cy);
                    const auto [z0, z1] = range(cz);
                    i32 level = -1;
                    bool mixed = false;
                    for (i32 z = z0; z <= z1; ++z) {
                        for (i32 y = y0; y <= y1; ++y) {
                            for (i32 x = x0; x <= x1; ++x) {
                                const Leaf& leaf = blocks[static_cast<usize>((z * 4 + y) * 4 + x)];
                                if (leaf.present) {
                                    mixed = mixed || (level >= 0 && level != leaf.coord.level);
                                    level = std::max<i32>(level, leaf.coord.level);
                                }
                            }
                        }
                    }
                    touching[static_cast<usize>((cz * 9 + cy) * 9 + cx)] = static_cast<i8>(level);
                    same_level[static_cast<usize>((cz * 9 + cy) * 9 + cx)] = !mixed &&
                        (level < 0 || level == self.level);
                }
            }
        }
    }

    void prepare(const LodLayout& lay, const ChunkSource& st, ChunkCoord leaf) {
        layout = &lay;
        store = &st;
        self = leaf;
        step = i64{1} << leaf.level;
        origin = {chunk_origin(leaf.x, leaf.level), chunk_origin(leaf.y, leaf.level),
                  chunk_origin(leaf.z, leaf.level)};
        half = chunk_extent(leaf.level) / 2;
        block2 = 2 * half;
        for (usize a = 0; a < 3; ++a) {
            base2[a] = 2 * (origin[a] - half);
        }
        for (i64 z = 0; z < 4; ++z) {
            for (i64 y = 0; y < 4; ++y) {
                for (i64 x = 0; x < 4; ++x) {
                    blocks[static_cast<usize>((z * 4 + y) * 4 + x)] =
                        make_leaf(layout->leaf_at2(base2[0] + x * block2 + half + 1,
                                                   base2[1] + y * block2 + half + 1,
                                                   base2[2] + z * block2 + half + 1));
                }
            }
        }
        classify();
        const Sample o = store->outside();
        outside = {dequantize(o.density), o.density > 0 ? o.material : air};
        foreign.clear();
    }

    // Alle Blätter der Nachbarschaft einheitlich und auf derselben Seite der Oberfläche.
    [[nodiscard]] bool trivially_empty() const noexcept {
        int sign = 0;
        for (const Leaf& leaf : blocks) {
            Sample value = store->outside();
            if (leaf.chunk != nullptr) {
                if (!leaf.chunk->uniform()) {
                    return false;
                }
                value = leaf.chunk->uniform_value();
            }
            const int s = value.density > 0 ? 1 : -1;
            if (sign != 0 && s != sign) {
                return false;
            }
            sign = s;
        }
        return true;
    }

    void fill_padded() {
        const f32 inv = 1.0f / static_cast<f32>(step);
        // Halber Chunk in eigenen Samples; der eigene Chunk belegt die Blöcke 1 und 2 pro Achse.
        constexpr i32 block = chunk_voxels / 2;
        // Pro Index -2 bis 34: Lage im Raster der halben Chunks (wie in field()) und Block, dem
        // das Sample gehört.
        std::array<i32, padded_lod> category{};
        std::array<i32, padded_lod> owner{};
        for (i32 i = -pad; i <= chunk_voxels + pad; ++i) {
            const i32 b = (i + block) / block;
            owner[static_cast<usize>(i + pad)] = b;
            category[static_cast<usize>(i + pad)] = (i + block) % block == 0 ? 2 * b : 2 * b + 1;
        }
        for (i32 z = -pad; z <= chunk_voxels + pad; ++z) {
            const i32 cz = category[static_cast<usize>(z + pad)];
            const i32 bz = owner[static_cast<usize>(z + pad)];
            for (i32 y = -pad; y <= chunk_voxels + pad; ++y) {
                const i32 cy = category[static_cast<usize>(y + pad)];
                const i32 by = owner[static_cast<usize>(y + pad)];
                for (i32 x = -pad; x <= chunk_voxels + pad; ++x) {
                    const i32 cx = category[static_cast<usize>(x + pad)];
                    const usize i = pindex(x, y, z);
                    // Schneller Weg: Alle Blätter, die den Punkt berühren, haben die eigene Stufe.
                    // Dann gilt das Sample des Blatts, dem er gehört.
                    if (same_level[static_cast<usize>((cz * 9 + cy) * 9 + cx)]) {
                        const Leaf& leaf = blocks[static_cast<usize>(
                            (bz * 4 + by) * 4 + owner[static_cast<usize>(x + pad)])];
                        if (leaf.chunk == nullptr) {
                            pd[i] = outside.d * inv;
                            pm[i] = outside.m;
                            continue;
                        }
                        const auto local = [&](i32 v, i32 c, usize axis) {
                            return static_cast<u32>(
                                v - static_cast<i32>((chunk_origin(c, self.level) - origin[axis]) /
                                                     step));
                        };
                        const Sample s = leaf.chunk->sample(local(x, leaf.coord.x, 0),
                                                            local(y, leaf.coord.y, 1),
                                                            local(z, leaf.coord.z, 2));
                        pd[i] = dequantize(s.density);
                        pm[i] = s.material;
                        continue;
                    }
                    const FieldValue v = field(origin[0] + x * step, origin[1] + y * step,
                                               origin[2] + z * step);
                    pd[i] = v.d * inv;
                    pm[i] = v.m;
                }
            }
        }
    }

    u32 emit(const CellVertex& v, Vec3 offset, f32 scale) {
        std::array<f32, palette_size> weights{};
        for (u32 c = 0; c < 8; ++c) {
            if (v.d[c] <= 0.0f) {
                continue;
            }
            const f32 near = v.d[c] + 0.25f;
            weights[palette_slot(*out, v.m[c])] += 1.0f / (near * near);
        }
        const auto [low, high] = pack_weights(weights);
        const Vec3 p = offset + v.local * scale;
        const auto index = static_cast<u32>(out->vertices.size());
        out->vertices.push_back({p.x, p.y, p.z, low, v.normal.x, v.normal.y, v.normal.z, high});
        return index;
    }

    // Eckpunkt einer Zelle in eigener Auflösung (Index -1 bis 31) aus dem Randgitter.
    u32 own_vertex(i32 x, i32 y, i32 z) {
        u32& slot = cells[cindex(x, y, z)];
        if (slot != vertex_pending) {
            return slot;
        }
        CellBlock block;
        for (u32 bz = 0; bz < 4; ++bz) {
            for (u32 by = 0; by < 4; ++by) {
                for (u32 bx = 0; bx < 4; ++bx) {
                    const usize i = pindex(x - 1 + static_cast<i32>(bx), y - 1 + static_cast<i32>(by),
                                           z - 1 + static_cast<i32>(bz));
                    block.d[CellBlock::index(bx, by, bz)] = pd[i];
                    block.m[CellBlock::index(bx, by, bz)] = pm[i];
                }
            }
        }
        const auto v = cell_vertex(block);
        slot = v ? emit(*v, Vec3{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)}, 1.0f)
                 : no_vertex;
        return slot;
    }

    // Eckpunkt einer Zelle eines anderen Blatts (andere Stufe) aus dem Feld.
    u32 foreign_vertex(const CellRef& cell) {
        const auto [it, inserted] = foreign.try_emplace(cell, no_vertex);
        if (!inserted) {
            return it->second;
        }
        const i64 s = i64{1} << cell.level;
        const f32 inv = 1.0f / static_cast<f32>(s);
        CellBlock block;
        for (u32 bz = 0; bz < 4; ++bz) {
            for (u32 by = 0; by < 4; ++by) {
                for (u32 bx = 0; bx < 4; ++bx) {
                    const FieldValue v = field(cell.x + (static_cast<i64>(bx) - 1) * s,
                                               cell.y + (static_cast<i64>(by) - 1) * s,
                                               cell.z + (static_cast<i64>(bz) - 1) * s);
                    block.d[CellBlock::index(bx, by, bz)] = v.d * inv;
                    block.m[CellBlock::index(bx, by, bz)] = v.m;
                }
            }
        }
        if (const auto v = cell_vertex(block)) {
            const auto to_own = [&](i64 value, usize axis) {
                return static_cast<f32>(value - origin[axis]) / static_cast<f32>(step);
            };
            it->second = emit(*v, Vec3{to_own(cell.x, 0), to_own(cell.y, 1), to_own(cell.z, 2)},
                              static_cast<f32>(s) / static_cast<f32>(step));
        }
        return it->second;
    }

    // Zelle am Punkt (verdoppelte Koordinaten, ungerade); ohne Blatt eine gedachte Zelle in
    // eigener Auflösung.
    CellRef cell_at2(i64 x2, i64 y2, i64 z2) const noexcept {
        const Leaf leaf = leaf_at2(x2, y2, z2);
        if (!leaf.present) {
            return {floor_div(x2, 2 * step) * step, floor_div(y2, 2 * step) * step,
                    floor_div(z2, 2 * step) * step, self.level, true};
        }
        const u8 l = leaf.coord.level;
        const i64 s = i64{1} << l;
        const auto axis = [&](i64 v2, i32 c) {
            const i64 o = chunk_origin(c, l);
            return o + floor_div(v2 - 2 * o, 2 * s) * s;
        };
        return {axis(x2, leaf.coord.x), axis(y2, leaf.coord.y), axis(z2, leaf.coord.z), l, false};
    }

    u32 vertex_of(const CellRef& cell) {
        if (cell.level == self.level) {
            const i64 x = (cell.x - origin[0]) / step;
            const i64 y = (cell.y - origin[1]) / step;
            const i64 z = (cell.z - origin[2]) / step;
            if (x >= -1 && x < chunk_voxels && y >= -1 && y < chunk_voxels && z >= -1 &&
                z < chunk_voxels) {
                return own_vertex(static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z));
            }
        }
        return foreign_vertex(cell);
    }

    void emit_polygon(std::span<const u32> v, bool flip) {
        if (v.size() == 4) {
            if (flip) {
                out->indices.insert(out->indices.end(), {v[0], v[3], v[2], v[0], v[2], v[1]});
            } else {
                out->indices.insert(out->indices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
            }
        } else if (flip) {
            out->indices.insert(out->indices.end(), {v[0], v[2], v[1]});
        } else {
            out->indices.insert(out->indices.end(), {v[0], v[1], v[2]});
        }
    }

    // Kanten einer Zelle am Rand des Blatts, deren Nachbarzellen in anderen Blättern liegen
    // können: Kandidaten auf den Linien durch die untere Ecke und durch die Mitten der unteren
    // Flächen, als ganze und halbe Kanten. Gültig ist ein Kandidat, wenn um ihn mindestens drei
    // verschiedene Zellen liegen und er so lang ist wie die kleinste davon (minimale Kante).
    void boundary_edges(i32 cx, i32 cy, i32 cz, u32 a) {
        const u32 b = (a + 1) % 3;
        const u32 c = (a + 2) % 3;
        const std::array<i64, 3> o{origin[0] + cx * step, origin[1] + cy * step,
                                   origin[2] + cz * step};
        const i64 h = step / 2;
        struct Line {
            i64 ob, oc;
        };
        struct Sub {
            i64 start, length;
        };
        std::array<Line, 3> lines{{{0, 0}, {h, 0}, {0, h}}};
        std::array<Sub, 3> subs{{{0, step}, {0, h}, {h, h}}};
        const usize line_count = self.level > 0 ? 3 : 1;
        const usize sub_count = self.level > 0 ? 3 : 1;
        constexpr std::array<std::array<i64, 2>, 4> around{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
        for (usize li = 0; li < line_count; ++li) {
            for (usize si = 0; si < sub_count; ++si) {
                std::array<i64, 3> e = o;
                e[b] += lines[li].ob;
                e[c] += lines[li].oc;
                e[a] += subs[si].start;
                const i64 length = subs[si].length;
                std::array<CellRef, 4> ring;
                for (usize k = 0; k < 4; ++k) {
                    std::array<i64, 3> q{2 * e[0], 2 * e[1], 2 * e[2]};
                    q[a] += length;
                    q[b] += around[k][0];
                    q[c] += around[k][1];
                    ring[k] = cell_at2(q[0], q[1], q[2]);
                }
                std::array<CellRef, 4> distinct;
                usize count = 0;
                for (usize k = 0; k < 4; ++k) {
                    if (!(ring[k] == ring[(k + 3) % 4])) {
                        distinct[count++] = ring[k];
                    }
                }
                if (count < 3) {
                    continue;
                }
                i64 smallest = step * 4;
                for (usize k = 0; k < count; ++k) {
                    smallest = std::min(smallest, i64{1} << distinct[k].level);
                }
                if (length != smallest) {
                    continue;
                }
                std::array<i64, 3> end = e;
                end[a] += length;
                const bool solid = field(e[0], e[1], e[2]).d > 0.0f;
                if (solid == (field(end[0], end[1], end[2]).d > 0.0f)) {
                    continue;
                }
                std::array<u32, 4> v{};
                bool complete = true;
                for (usize k = 0; k < count; ++k) {
                    v[k] = vertex_of(distinct[k]);
                    complete = complete && v[k] < vertex_pending;
                }
                if (complete) {
                    emit_polygon(std::span{v.data(), count}, !solid);
                }
            }
        }
    }

    void build(Mesh& mesh) {
        out = &mesh;
        mesh.clear();
        if (trivially_empty()) {
            return;
        }
        fill_padded();
        std::fill(cells.begin(), cells.end(), vertex_pending);
        const auto pd_at = [&](i32 x, i32 y, i32 z) { return pd[pindex(x, y, z)]; };
        for (i32 z = 0; z < chunk_voxels; ++z) {
            for (i32 y = 0; y < chunk_voxels; ++y) {
                for (i32 x = 0; x < chunk_voxels; ++x) {
                    // Schneller Test über die Vorzeichen der acht Ecken.
                    u32 inside = 0;
                    for (u32 k = 0; k < 8; ++k) {
                        inside |= (pd_at(x + static_cast<i32>(k & 1u),
                                         y + static_cast<i32>((k >> 1) & 1u),
                                         z + static_cast<i32>((k >> 2) & 1u)) > 0.0f
                                       ? 1u
                                       : 0u)
                                  << k;
                    }
                    if (inside == 0 || inside == 0xFF) {
                        cells[cindex(x, y, z)] = no_vertex;
                        continue;
                    }
                    const u32 vertex = own_vertex(x, y, z);
                    const std::array<i32, 3> idx{x, y, z};
                    const bool solid = (inside & 1u) != 0;
                    for (u32 a = 0; a < 3; ++a) {
                        const u32 b = (a + 1) % 3;
                        const u32 c = (a + 2) % 3;
                        if (idx[b] < 1 || idx[c] < 1) {
                            boundary_edges(x, y, z, a);
                            continue;
                        }
                        // Innere Kante an Ecke 0: alle vier Zellen gehören dem Blatt.
                        if (solid == ((inside & (1u << (1u << a))) != 0)) {
                            continue;
                        }
                        std::array<i32, 3> mb = idx;
                        mb[b] -= 1;
                        std::array<i32, 3> mc = idx;
                        mc[c] -= 1;
                        std::array<i32, 3> mbc = mb;
                        mbc[c] -= 1;
                        const std::array<u32, 4> v{own_vertex(mbc[0], mbc[1], mbc[2]),
                                                   own_vertex(mc[0], mc[1], mc[2]), vertex,
                                                   own_vertex(mb[0], mb[1], mb[2])};
                        if (v[0] < vertex_pending && v[1] < vertex_pending &&
                            v[3] < vertex_pending) {
                            emit_polygon(v, !solid);
                        }
                    }
                }
            }
        }
    }
};

LodMesher::LodMesher() : impl_{std::make_unique<Impl>()} {}
LodMesher::~LodMesher() = default;
LodMesher::LodMesher(LodMesher&&) noexcept = default;
LodMesher& LodMesher::operator=(LodMesher&&) noexcept = default;

namespace {

// Liest direkt aus dem ChunkStore.
class StoreSource final : public ChunkSource {
public:
    explicit StoreSource(const ChunkStore& store) noexcept : store_{&store} {}
    [[nodiscard]] const Chunk* find(ChunkCoord coord) const noexcept override {
        return store_->find(coord);
    }
    [[nodiscard]] Sample outside() const noexcept override {
        return store_->outside();
    }

private:
    const ChunkStore* store_;
};

} // namespace

void LodMesher::build(const LodLayout& layout, const ChunkStore& store, ChunkCoord leaf,
                      Mesh& out) {
    const StoreSource source{store};
    build(layout, source, leaf, out);
}

void LodMesher::build(const LodLayout& layout, const ChunkSource& source, ChunkCoord leaf,
                      Mesh& out) {
    NGINE_PROFILE_ZONE("voxel::LodMesher::build");
    impl_->prepare(layout, source, leaf);
    impl_->build(out);
    build_meshlets(out);
}

} // namespace ngine::voxel
