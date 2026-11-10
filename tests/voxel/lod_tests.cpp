#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <map>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.voxel;

// Baum der Chunks in Stufen und Netze mit Nähten zwischen den Stufen (R3.3, ADR 0041).

using namespace ngine;
using math::DVec3;

namespace {

// Netze aller Blätter in Koordinaten des Terrains (Voxel), Eckpunkte verschweißt.
struct Surface {
    std::vector<DVec3> points;
    std::vector<std::array<u32, 3>> triangles;
    std::unordered_map<i64, std::vector<u32>> buckets;

    static i64 key(i64 x, i64 y, i64 z) {
        return (x * 73856093) ^ (y * 19349663) ^ (z * 83492791);
    }

    u32 weld(DVec3 p) {
        const auto cell = [](f64 v) { return static_cast<i64>(std::floor(v * 16.0)); };
        const i64 cx = cell(p.x);
        const i64 cy = cell(p.y);
        const i64 cz = cell(p.z);
        for (i64 z = cz - 1; z <= cz + 1; ++z) {
            for (i64 y = cy - 1; y <= cy + 1; ++y) {
                for (i64 x = cx - 1; x <= cx + 1; ++x) {
                    const auto it = buckets.find(key(x, y, z));
                    if (it == buckets.end()) {
                        continue;
                    }
                    for (const u32 i : it->second) {
                        const DVec3 d = points[i] - p;
                        if (std::abs(d.x) < 1e-3 && std::abs(d.y) < 1e-3 && std::abs(d.z) < 1e-3) {
                            return i;
                        }
                    }
                }
            }
        }
        const auto index = static_cast<u32>(points.size());
        points.push_back(p);
        buckets[key(cx, cy, cz)].push_back(index);
        return index;
    }

    void add(voxel::ChunkCoord coord, const voxel::Mesh& mesh) {
        const auto step = static_cast<f64>(i64{1} << coord.level);
        const DVec3 origin{static_cast<f64>(voxel::chunk_origin(coord.x, coord.level)),
                           static_cast<f64>(voxel::chunk_origin(coord.y, coord.level)),
                           static_cast<f64>(voxel::chunk_origin(coord.z, coord.level))};
        std::vector<u32> map(mesh.vertices.size());
        for (usize i = 0; i < mesh.vertices.size(); ++i) {
            const voxel::Vertex& v = mesh.vertices[i];
            map[i] = weld(origin + DVec3{v.px, v.py, v.pz} * step);
        }
        for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const std::array<u32, 3> t{map[mesh.indices[i]], map[mesh.indices[i + 1]],
                                       map[mesh.indices[i + 2]]};
            if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) {
                triangles.push_back(t);
            }
        }
    }

    // Gerichtete Kanten, die nicht ebenso oft in Gegenrichtung vorkommen: offen (Riss) oder falsch
    // orientiert. Zwei Zellen, deren Eckpunkte auf dieselbe Ecke fallen (Sample genau 0), ergeben
    // doppelte Dreiecke; das ist ein Punkt, kein Riss, und zählt nicht. accept filtert, welche
    // Kanten zählen (z. B. nicht am Rand der Welt).
    template <typename F>
    std::vector<std::pair<u32, u32>> open_edges(F&& accept) const {
        std::map<std::pair<u32, u32>, int> count;
        for (const auto& t : triangles) {
            for (u32 k = 0; k < 3; ++k) {
                ++count[{t[k], t[(k + 1) % 3]}];
            }
        }
        std::vector<std::pair<u32, u32>> result;
        for (const auto& [edge, n] : count) {
            const auto back = count.find({edge.second, edge.first});
            const bool closed = back != count.end() && back->second == n;
            if (!closed && accept(points[edge.first], points[edge.second])) {
                result.push_back(edge);
            }
        }
        return result;
    }
};

// Füllt jedes Blatt in seiner Auflösung aus einer Abstandsfunktion in Voxeln.
template <typename F>
void fill_leaves(voxel::ChunkStore& store, const voxel::LodLayout& layout, F&& distance) {
    for (const voxel::ChunkCoord c : layout.leaves()) {
        voxel::Chunk& chunk = store.get_or_create(c);
        const i64 step = i64{1} << c.level;
        const i64 ox = voxel::chunk_origin(c.x, c.level);
        const i64 oy = voxel::chunk_origin(c.y, c.level);
        const i64 oz = voxel::chunk_origin(c.z, c.level);
        for (u32 z = 0; z < 32; ++z) {
            for (u32 y = 0; y < 32; ++y) {
                for (u32 x = 0; x < 32; ++x) {
                    const DVec3 p{static_cast<f64>(ox + x * step), static_cast<f64>(oy + y * step),
                                  static_cast<f64>(oz + z * step)};
                    const f64 d = distance(p);
                    const i8 density =
                        voxel::quantize(static_cast<f32>(d / static_cast<f64>(step)));
                    chunk.set(x, y, z,
                              {density, density > 0 ? (p.y > 0.0 ? voxel::grass : voxel::rock)
                                                    : voxel::air});
                }
            }
        }
        chunk.compact();
    }
}

Surface mesh_all(jobs::JobSystem& jobs, const voxel::LodLayout& layout,
                 const voxel::ChunkStore& store) {
    std::vector<voxel::Mesh> meshes(layout.size());
    voxel::mesh_leaves(jobs, layout, store, layout.leaves(), meshes);
    Surface surface;
    for (usize i = 0; i < meshes.size(); ++i) {
        surface.add(layout.leaves()[i], meshes[i]);
    }
    return surface;
}

} // namespace

TEST_CASE("LOD selection is fine near observers, coarse far away and balanced", "[voxel]") {
    const voxel::LodDesc desc{.min_chunk_y = 0, .max_chunk_y = 4, .view_distance = 4000.0,
                              .detail = 2.0};
    const std::array observers{DVec3{100.0, 50.0, -30.0}};
    const voxel::LodLayout layout = voxel::select_lod(observers, desc);
    REQUIRE(layout.size() > 0);
    CHECK(layout.balanced());
    CHECK(voxel::root_level(desc) == 6);

    const auto counts = layout.level_counts();
    CHECK(counts[0] > 0);
    CHECK(counts[5] > 0);
    // Der Beobachter steht in einem Blatt der Stufe 0.
    const auto here = layout.leaf_of(100, 50, -30);
    REQUIRE(here);
    CHECK(here->level == 0);
    // Blätter überlappen sich nicht: Jedes Sample eines Blatts gehört genau diesem Blatt.
    for (const voxel::ChunkCoord c : layout.leaves()) {
        const voxel::SampleBox b = voxel::chunk_box(c);
        CHECK(layout.leaf_of(b.min_x, b.min_y, b.min_z) == c);
        CHECK(layout.leaf_of(b.max_x - 1, b.max_y - 1, b.max_z - 1) == c);
        // Ein Blatt der Stufe n liegt mindestens detail × seine Breite vom Beobachter entfernt,
        // es sei denn, es ist ein Blatt der Stufe 0 oder fürs Ausgleichen geteilt.
        if (c.level > 0) {
            CHECK(voxel::observer_distance(observers, c) >=
                  desc.detail * static_cast<f64>(voxel::chunk_extent(c.level)) * 0.49);
        }
    }
    // Die Ferne ist grob: Rund 2 km Sicht kommen mit wenigen hundert Chunks aus.
    CHECK(layout.size() < 2000);

    // Zwei Beobachter weit auseinander: beide stehen in feinen Blättern.
    const std::array two{DVec3{0.0, 40.0, 0.0}, DVec3{3000.0, 40.0, 0.0}};
    const voxel::LodLayout both = voxel::select_lod(two, desc);
    CHECK(both.balanced());
    CHECK(both.leaf_of(0, 40, 0)->level == 0);
    CHECK(both.leaf_of(3000, 40, 0)->level == 0);
}

TEST_CASE("LOD mesher on one level matches the chunk mesher", "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    std::vector<voxel::ChunkCoord> coords;
    for (i32 z = -2; z < 2; ++z) {
        for (i32 y = 0; y < 3; ++y) {
            for (i32 x = -2; x < 2; ++x) {
                coords.push_back({x, y, z, 0});
            }
        }
    }
    voxel::generate_chunks(jobs, store, coords, voxel::Generator{{.seed = 7}});
    static_cast<void>(store.take_changes());

    const voxel::LodLayout layout{store.coords()};
    std::vector<voxel::Mesh> lod(coords.size());
    std::vector<voxel::Mesh> plain(coords.size());
    voxel::mesh_leaves(jobs, layout, store, coords, lod);
    voxel::mesh_chunks(jobs, store, coords, plain);
    usize triangles = 0;
    for (usize i = 0; i < coords.size(); ++i) {
        // Gleiche Dreiecke; der Chunk-Mesher legt zusätzlich ungenutzte Eckpunkte im Rand an.
        CHECK(lod[i].indices.size() == plain[i].indices.size());
        CHECK(lod[i].vertices.size() <= plain[i].vertices.size());
        triangles += lod[i].triangle_count();
    }
    CHECK(triangles > 1000);
}

TEST_CASE("A sphere across several LOD levels has no open edges", "[voxel]") {
    jobs::JobSystem jobs;
    const voxel::LodDesc desc{.min_chunk_y = -8, .max_chunk_y = 8, .view_distance = 400.0,
                              .detail = 1.5};
    const std::array observers{DVec3{3.0, 2.0, 5.0}};
    const voxel::LodLayout layout = voxel::select_lod(observers, desc);
    REQUIRE(layout.balanced());
    const auto counts = layout.level_counts();
    CHECK(counts[0] > 0);
    CHECK(counts[1] > 0);
    CHECK(counts[2] > 0);

    for (const auto& [center, radius] : {std::pair{DVec3{5.3, 3.1, 7.7}, 90.0},
                                         std::pair{DVec3{-20.0, 10.5, 33.0}, 47.0},
                                         std::pair{DVec3{60.0, -40.0, -64.0}, 130.0}}) {
        voxel::ChunkStore store;
        fill_leaves(store, layout,
                    [&](DVec3 p) { return radius - math::length(p - center); });
        const Surface surface = mesh_all(jobs, layout, store);
        CHECK(surface.triangles.size() > 1000);
        const auto open = surface.open_edges([](DVec3, DVec3) { return true; });
        CHECK(open.empty());
    }
}

TEST_CASE("Generated terrain across LOD levels has no open edges inside the world", "[voxel]") {
    jobs::JobSystem jobs;
    const voxel::LodDesc desc{.min_chunk_y = 0, .max_chunk_y = 4, .view_distance = 700.0,
                              .detail = 1.5};
    const std::array observers{DVec3{40.0, 60.0, -25.0}};
    const voxel::LodLayout layout = voxel::select_lod(observers, desc);
    REQUIRE(layout.balanced());
    CHECK(layout.level_counts()[3] > 0);

    voxel::ChunkStore store;
    voxel::generate_chunks(jobs, store, layout.leaves(), voxel::Generator{{.seed = 3}});
    const Surface surface = mesh_all(jobs, layout, store);
    CHECK(surface.triangles.size() > 10000);
    // Am Rand der Welt (Sichtweite, unter y = 0, über dem senkrechten Bereich) schließt das Netz
    // gegen gedachte Zellen und darf offen sein; überall sonst nicht.
    const auto inside_world = [&](DVec3 a, DVec3 b) {
        const DVec3 m = (a + b) * 0.5;
        const auto leaf = layout.leaf_of(static_cast<i64>(std::floor(m.x)),
                                         static_cast<i64>(std::floor(m.y)),
                                         static_cast<i64>(std::floor(m.z)));
        if (!leaf) {
            return false;
        }
        const f64 r = 3.0 * static_cast<f64>(i64{1} << leaf->level) + 1.0;
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const DVec3 q = m + DVec3{dx * r, dy * r, dz * r};
                    if (!layout.leaf_of(static_cast<i64>(std::floor(q.x)),
                                        static_cast<i64>(std::floor(q.y)),
                                        static_cast<i64>(std::floor(q.z)))) {
                        return false;
                    }
                }
            }
        }
        return true;
    };
    const auto open = surface.open_edges(inside_world);
    for (usize i = 0; i < std::min<usize>(open.size(), 12); ++i) {
        const DVec3 a = surface.points[open[i].first];
        const DVec3 b = surface.points[open[i].second];
        const auto leaf = layout.leaf_of(static_cast<i64>(std::floor(a.x)),
                                         static_cast<i64>(std::floor(a.y)),
                                         static_cast<i64>(std::floor(a.z)));
        UNSCOPED_INFO("offen: " << a.x << " " << a.y << " " << a.z << " -> " << b.x << " " << b.y
                                << " " << b.z << " Blatt " << leaf->x << " " << leaf->y << " "
                                << leaf->z << " Stufe " << int{leaf->level});
    }
    CHECK(open.empty());
}

TEST_CASE("Coarse generator levels sample the same world", "[voxel]") {
    const voxel::Generator generator{{.seed = 11}};
    for (const u8 level : {u8{1}, u8{2}, u8{4}}) {
        const voxel::ChunkCoord coord{-1, 0, 1, level};
        voxel::Chunk chunk;
        generator.fill(coord, chunk);
        const i64 step = i64{1} << level;
        const i64 ox = voxel::chunk_origin(coord.x, level);
        const i64 oz = voxel::chunk_origin(coord.z, level);
        for (u32 z = 0; z < 32; z += 5) {
            for (u32 y = 0; y < 32; y += 3) {
                for (u32 x = 0; x < 32; x += 7) {
                    CHECK(chunk.sample(x, y, z) ==
                          generator.sample(ox + x * step, y * step, oz + z * step, level));
                }
            }
        }
    }
    // Grob und fein liegen an gemeinsamen Punkten meist auf derselben Seite der Oberfläche.
    int same = 0;
    int total = 0;
    for (i64 z = -200; z < 200; z += 8) {
        for (i64 x = -200; x < 200; x += 8) {
            for (i64 y = 0; y < 120; y += 8) {
                const bool fine = generator.sample(x, y, z).density > 0;
                const bool coarse = generator.sample(x, y, z, 3).density > 0;
                same += fine == coarse ? 1 : 0;
                ++total;
            }
        }
    }
    CHECK(same > total * 9 / 10);
}

namespace {

using MeshMap = std::map<voxel::ChunkCoord, voxel::Mesh>;

void apply(MeshMap& meshes, voxel::StreamChanges changes) {
    for (const voxel::ChunkCoord c : changes.removed) {
        meshes.erase(c);
    }
    for (auto& [coord, mesh] : changes.meshes) {
        meshes[coord] = std::move(mesh);
    }
}

bool same_mesh(const voxel::Mesh& a, const voxel::Mesh& b) {
    if (a.indices != b.indices || a.vertices.size() != b.vertices.size()) {
        return false;
    }
    for (usize i = 0; i < a.vertices.size(); ++i) {
        const voxel::Vertex& p = a.vertices[i];
        const voxel::Vertex& q = b.vertices[i];
        if (p.px != q.px || p.py != q.py || p.pz != q.pz || p.weights_low != q.weights_low) {
            return false;
        }
    }
    return true;
}

// Die Netze des Streamers sind genau die, die ein frisches Meshing der aktuellen Auswahl ergäbe:
// Kein Blatt fehlt, keines ist veraltet, keines gehört nicht zur Auswahl.
int stale_meshes(jobs::JobSystem& jobs, const voxel::Streamer& streamer,
                 const voxel::ChunkStore& store, const MeshMap& meshes) {
    const voxel::LodLayout& layout = streamer.layout();
    std::vector<voxel::Mesh> fresh(layout.size());
    voxel::mesh_leaves(jobs, layout, store, layout.leaves(), fresh);
    int stale = 0;
    for (usize i = 0; i < layout.size(); ++i) {
        const auto it = meshes.find(layout.leaves()[i]);
        if (it == meshes.end() || !same_mesh(it->second, fresh[i])) {
            ++stale;
        }
    }
    for (const auto& entry : meshes) {
        stale += layout.contains(entry.first) ? 0 : 1;
    }
    return stale;
}

constexpr voxel::StreamDesc test_stream{
    .lod = {.min_chunk_y = 0, .max_chunk_y = 4, .view_distance = 600.0, .detail = 1.5}};

} // namespace

TEST_CASE("Streaming follows an observer and keeps meshes and memory consistent", "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    voxel::Streamer streamer{{.seed = 5}, test_stream};
    MeshMap meshes;

    std::array observer{DVec3{0.0, 60.0, 0.0}};
    apply(meshes, streamer.update(jobs, store, observer, true));
    REQUIRE(streamer.idle());
    REQUIRE(streamer.layout().size() > 0);
    CHECK(streamer.layout().balanced());
    CHECK(store.size() == streamer.layout().size());
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
    const usize start_leaves = streamer.layout().size();

    // Flug über 2 000 Voxel (1 km) in Schritten: Auswahl, Chunks und Netze bleiben stimmig, der
    // Speicher bleibt in derselben Größenordnung.
    usize min_leaves = start_leaves;
    usize max_leaves = start_leaves;
    for (int step = 1; step <= 20; ++step) {
        observer[0].x = step * 100.0;
        observer[0].z = step * 37.0;
        apply(meshes, streamer.update(jobs, store, observer, true));
        CHECK(streamer.idle());
        CHECK(streamer.layout().balanced());
        CHECK(store.size() == streamer.layout().size());
        min_leaves = std::min(min_leaves, streamer.layout().size());
        max_leaves = std::max(max_leaves, streamer.layout().size());
        if (step % 5 == 0) {
            CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
        }
    }
    CHECK(streamer.layout().leaf_of(2000, 60, 740)->level == 0);
    CHECK(max_leaves <= min_leaves * 3 / 2);
    CHECK(streamer.stats().commits >= 20);
    CHECK(streamer.stats().generated_total > start_leaves);
}

TEST_CASE("Streaming without waiting commits only complete selections", "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    voxel::StreamDesc desc = test_stream;
    desc.max_generating = 3;
    voxel::Streamer streamer{{.seed = 9}, desc};
    MeshMap meshes;
    std::array observer{DVec3{-300.0, 50.0, 120.0}};
    u64 commits = 0;
    int frames = 0;
    // Frames von rund 1 ms; die Jobs erzeugen im Hintergrund.
    // Die Jobs brauchen Zeit, nicht Frames: Grenze nach Uhrzeit, großzügig für ThreadSanitizer.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{10};
    bool finished = false;
    for (; std::chrono::steady_clock::now() < deadline; ++frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        if (frames == 40) {
            observer[0].x += 150.0;
        }
        apply(meshes, streamer.update(jobs, store, observer));
        // Höchstens das Budget gleichzeitig in Jobs.
        CHECK(streamer.stats().generating <= 3);
        if (streamer.stats().commits != commits) {
            commits = streamer.stats().commits;
            // Nach jeder Übernahme passen Netze und Chunks zur Auswahl.
            CHECK(streamer.layout().balanced());
            CHECK(store.size() == streamer.layout().size());
            CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
        }
        if (frames > 40 && streamer.idle()) {
            finished = true;
            break;
        }
    }
    CHECK(finished);
    CHECK(commits >= 2);
    CHECK(streamer.layout().leaf_of(-150, 50, 120)->level == 0);
}

TEST_CASE("Streaming around several observers and remeshing brush edits", "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    voxel::Streamer streamer{{.seed = 2}, test_stream};
    MeshMap meshes;
    const std::array observers{DVec3{0.0, 50.0, 0.0}, DVec3{1500.0, 50.0, -900.0},
                               DVec3{-800.0, 50.0, 700.0}};
    apply(meshes, streamer.update(jobs, store, observers, true));
    for (const DVec3& o : observers) {
        CHECK(streamer.layout().leaf_of(static_cast<i64>(o.x), static_cast<i64>(o.y),
                                        static_cast<i64>(o.z))
                  ->level == 0);
    }

    // Ein Krater an der Oberfläche beim ersten Beobachter: betroffene Netze werden im selben
    // Aufruf neu gemeshed, auch über Blattgrenzen hinweg.
    const voxel::Generator generator{{.seed = 2}};
    const auto ground = static_cast<f32>(generator.height(0, 0));
    const voxel::SampleBox box =
        voxel::apply(store, {.mode = voxel::BrushMode::Remove, .center = {0.0f, ground, 0.0f},
                             .radius = 6.0f});
    REQUIRE_FALSE(box.empty());
    const voxel::StreamChanges changes = streamer.update(jobs, store, observers);
    CHECK(changes.meshes.size() >= 4);
    apply(meshes, std::move(const_cast<voxel::StreamChanges&>(changes)));
    CHECK(!store.has_changes());
    CHECK(store.size() == streamer.layout().size());
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
}

TEST_CASE("Brush edits while streaming in the background stay in the meshes", "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    voxel::Streamer streamer{{.seed = 5}, test_stream};
    MeshMap meshes;
    std::array observer{DVec3{0.0, 50.0, 0.0}};
    apply(meshes, streamer.update(jobs, store, observer, true));
    const voxel::Generator generator{{.seed = 5}};
    u64 commits = streamer.stats().commits;
    int frames = 0;
    // Der Beobachter fliegt, jeder Frame trägt vor ihm ab, nahe am Rand der feinsten Stufe. Dort
    // meshen die Jobs gerade Blätter neu, deren Nachbarn feiner werden.
    // Die Jobs brauchen Zeit, nicht Frames: Grenze nach Uhrzeit, großzügig für ThreadSanitizer.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{10};
    bool finished = false;
    for (; std::chrono::steady_clock::now() < deadline; ++frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        if (frames < 300) {
            observer[0].x += 1.0;
        }
        const f32 x = static_cast<f32>(observer[0].x) + 24.0f + static_cast<f32>(frames % 5) * 8.0f;
        const auto ground = static_cast<f32>(generator.height(static_cast<i64>(x), 0));
        static_cast<void>(voxel::apply(store, {.mode = voxel::BrushMode::Remove,
                                               .center = {x, ground, 0.0f},
                                               .radius = 5.0f}));
        apply(meshes, streamer.update(jobs, store, observer));
        CHECK(!store.has_changes());
        if (streamer.stats().commits != commits) {
            commits = streamer.stats().commits;
            CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
        }
        if (frames > 300 && streamer.idle()) {
            finished = true;
            break;
        }
    }
    CHECK(finished);
    CHECK(streamer.stats().commits >= 3);
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
}

TEST_CASE("A streamer can be destroyed while chunks are generated", "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    {
        voxel::Streamer streamer{{.seed = 4}, test_stream};
        const std::array observer{DVec3{0.0, 60.0, 0.0}};
        static_cast<void>(streamer.update(jobs, store, observer));
        CHECK_FALSE(streamer.idle());
    }
    CHECK(store.size() == 0);
}
