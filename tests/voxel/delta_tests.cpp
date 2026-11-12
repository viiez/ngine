#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.voxel;

// Deltas gegenüber der Grundwelt (R3.4, ADR 0042): Kodierung, Einrechnen in alle Stufen,
// Streaming mit Änderungen, Undo und Regionen-Dateien. Dazu die Pinsel Ebnen und Bemalen.

using namespace ngine;
using math::DVec3;

namespace {

constexpr voxel::GeneratorDesc test_world{.seed = 7};

constexpr voxel::StreamDesc test_stream{
    .lod = {.min_chunk_y = 0, .max_chunk_y = 4, .view_distance = 900.0, .detail = 1.5}};

using MeshMap = std::map<voxel::ChunkCoord, voxel::Mesh>;

void apply(MeshMap& meshes, voxel::StreamChanges changes) {
    for (const voxel::ChunkCoord c : changes.removed) {
        meshes.erase(c);
    }
    for (auto& [coord, mesh] : changes.meshes) {
        meshes[coord] = std::move(mesh);
    }
}

bool same_samples(const voxel::Chunk& a, const voxel::Chunk& b) {
    return voxel::checksum(a) == voxel::checksum(b);
}

// Netze des Streamers gleich einem frischen Meshing der Auswahl (siehe lod_tests.cpp).
int stale_meshes(jobs::JobSystem& jobs, const voxel::Streamer& streamer,
                 const voxel::ChunkStore& store, const MeshMap& meshes) {
    const voxel::LodLayout& layout = streamer.layout();
    std::vector<voxel::Mesh> fresh(layout.size());
    voxel::mesh_leaves(jobs, layout, store, layout.leaves(), fresh);
    int stale = 0;
    for (usize i = 0; i < layout.size(); ++i) {
        const auto it = meshes.find(layout.leaves()[i]);
        if (it == meshes.end() || it->second.indices != fresh[i].indices ||
            it->second.vertices.size() != fresh[i].vertices.size()) {
            ++stale;
        }
    }
    return stale;
}

// Wartet, bis der Streamer zur Auswahl passt und keine groben Blätter mehr neu erzeugt.
void settle(jobs::JobSystem& jobs, voxel::Streamer& streamer, voxel::ChunkStore& store,
            std::span<const DVec3> observers, MeshMap& meshes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{10};
    do {
        apply(meshes, streamer.update(jobs, store, observers, true));
    } while (!streamer.idle() && std::chrono::steady_clock::now() < deadline);
    // Neu zu erzeugende grobe Blätter starten erst im nächsten Aufruf.
    apply(meshes, streamer.update(jobs, store, observers, true));
}

} // namespace

TEST_CASE("Chunk deltas encode only the changed samples", "[voxel]") {
    const voxel::Generator generator{test_world};
    const voxel::ChunkCoord coord{0, 1, 0, 0};
    voxel::Chunk base;
    generator.fill(coord, base);
    REQUIRE_FALSE(base.uniform());

    // Gleiche Chunks: leeres Delta.
    CHECK(voxel::ChunkDelta::diff(base, base).empty());
    CHECK(voxel::ChunkDelta::diff(base, base).bytes().empty());

    // Ein Krater in der Mitte.
    voxel::ChunkStore store;
    store.get_or_create(coord) = base;
    const auto ground = static_cast<f32>(generator.height(16, 16));
    const voxel::SampleBox box = voxel::apply(
        store,
        {.mode = voxel::BrushMode::Remove, .center = {16.0f, ground, 16.0f}, .radius = 5.0f});
    REQUIRE_FALSE(box.empty());
    const voxel::Chunk& edited = *store.find(coord);
    const voxel::ChunkDelta delta = voxel::ChunkDelta::diff(base, edited);
    CHECK_FALSE(delta.empty());
    CHECK(delta.changed() > 100);
    CHECK(delta.changed() < 2000);
    // Kompakt: kaum mehr als 2 Bytes pro geändertes Sample, weit unter 64 KiB.
    CHECK(delta.bytes().size() < delta.changed() * 2 + 600);

    voxel::Chunk rebuilt = base;
    delta.apply(rebuilt);
    CHECK(same_samples(rebuilt, edited));

    // Bytes und zurück.
    const auto decoded = voxel::ChunkDelta::decode(delta.bytes());
    REQUIRE(decoded);
    CHECK(*decoded == delta);
    CHECK(decoded->changed() == delta.changed());

    // Abgeschnittene oder zu lange Deltas werden abgelehnt.
    CHECK_FALSE(voxel::ChunkDelta::decode(delta.bytes().first(delta.bytes().size() - 1)));
    std::vector<std::byte> too_long{std::byte{0xFF}, std::byte{0xFF}, std::byte{0x03},
                                    std::byte{0x01}, std::byte{0x00}, std::byte{0x00}};
    CHECK_FALSE(voxel::ChunkDelta::decode(too_long));
}

TEST_CASE("Deltas flow into coarse levels on their grid", "[voxel]") {
    voxel::TerrainDeltas deltas{test_world};
    voxel::ChunkStore store;
    const voxel::Generator& generator = deltas.generator();
    // Ein großer Krater um (64, h, 64): Pinsel auf Chunks, die nicht geladen sind.
    const auto ground = static_cast<f32>(generator.height(64, 64));
    const voxel::TerrainEdit edit = deltas.apply(
        store,
        {.mode = voxel::BrushMode::Remove, .center = {64.0f, ground, 64.0f}, .radius = 14.0f});
    REQUIRE_FALSE(edit.empty());
    CHECK(store.size() == 0);
    CHECK(deltas.size() == edit.changes.size());
    CHECK(deltas.size() >= 2);

    // Auf Stufe 0, 1 und 2 ist die Mitte des Kraters Luft, wo vorher Stoff war.
    const auto center_y = static_cast<i64>(ground) - 4;
    for (u8 level = 0; level <= 2; ++level) {
        const i64 step = i64{1} << level;
        const i64 y = center_y / step * step;
        const voxel::ChunkCoord coord{
            static_cast<i32>(voxel::floor_div(64, voxel::chunk_extent(level))),
            static_cast<i32>(voxel::floor_div(y, voxel::chunk_extent(level))),
            static_cast<i32>(voxel::floor_div(64, voxel::chunk_extent(level))), level};
        voxel::Chunk base;
        generator.fill(coord, base);
        voxel::Chunk with_deltas;
        deltas.build(coord, with_deltas);
        const voxel::SampleBox box = voxel::chunk_box(coord);
        const auto local = [&](i64 v, i64 min) { return static_cast<u32>((v - min) / step); };
        const voxel::Sample before =
            base.sample(local(64, box.min_x), local(y, box.min_y), local(64, box.min_z));
        const voxel::Sample after =
            with_deltas.sample(local(64, box.min_x), local(y, box.min_y), local(64, box.min_z));
        INFO("Stufe " << int{level});
        CHECK(before.density > 0);
        CHECK(after.density < 0);
        CHECK(after.material == voxel::air);
        CHECK_FALSE(same_samples(base, with_deltas));
    }

    // Weit weg ändert sich nichts.
    voxel::Chunk far_base;
    generator.fill({10, 0, 10, 1}, far_base);
    voxel::Chunk far_built;
    deltas.build({10, 0, 10, 1}, far_built);
    CHECK(same_samples(far_base, far_built));
}

TEST_CASE("Restoring deltas undoes and redoes a brush", "[voxel]") {
    voxel::TerrainDeltas deltas{test_world};
    voxel::ChunkStore store;
    // Zwei geladene Chunks, der Pinsel trifft auch nicht geladene Nachbarn.
    for (const voxel::ChunkCoord c :
         {voxel::ChunkCoord{0, 0, 0, 0}, voxel::ChunkCoord{0, 1, 0, 0}}) {
        deltas.build(c, store.get_or_create(c));
    }
    static_cast<void>(store.take_changes());
    const voxel::Chunk original = *store.find({0, 1, 0, 0});
    const auto ground = static_cast<f32>(deltas.generator().height(30, 30));
    const voxel::TerrainEdit edit = deltas.apply(store, {.mode = voxel::BrushMode::Add,
                                                         .center = {30.0f, ground + 2.0f, 30.0f},
                                                         .radius = 6.0f,
                                                         .material = voxel::sand});
    REQUIRE_FALSE(edit.empty());
    CHECK(store.has_changes());
    const voxel::Chunk edited = *store.find({0, 1, 0, 0});
    CHECK_FALSE(same_samples(original, edited));
    const u64 version = deltas.version();

    static_cast<void>(store.take_changes());
    const voxel::SampleBox undone = deltas.restore(store, edit.changes, false);
    CHECK_FALSE(undone.empty());
    CHECK(store.has_changes());
    CHECK(deltas.size() == 0);
    CHECK(same_samples(*store.find({0, 1, 0, 0}), original));
    CHECK(deltas.version() > version);

    const voxel::SampleBox redone = deltas.restore(store, edit.changes, true);
    CHECK_FALSE(redone.empty());
    CHECK(same_samples(*store.find({0, 1, 0, 0}), edited));
    // Schon so: nichts zu tun.
    CHECK(deltas.restore(store, edit.changes, true).empty());

    // Ein Terrain fester Größe ändert nur, was geladen ist.
    voxel::TerrainDeltas fixed{test_world};
    voxel::ChunkStore fixed_store;
    fixed.build({0, 1, 0, 0}, fixed_store.get_or_create({0, 1, 0, 0}));
    const voxel::TerrainEdit fixed_edit = fixed.apply(
        fixed_store,
        {.mode = voxel::BrushMode::Remove, .center = {31.0f, ground, 31.0f}, .radius = 6.0f}, true);
    REQUIRE_FALSE(fixed_edit.empty());
    for (const voxel::DeltaChange& change : fixed_edit.changes) {
        CHECK(change.coord == voxel::ChunkCoord{0, 1, 0, 0});
    }
    CHECK(fixed_store.size() == 1);
}

TEST_CASE("Flatten levels the ground and paint changes only materials", "[voxel]") {
    voxel::ChunkStore store;
    // Schräge Ebene: Höhe 10 + x/4 Voxel.
    for (i64 z = -4; z < 36; ++z) {
        for (i64 y = 0; y < 32; ++y) {
            for (i64 x = -4; x < 36; ++x) {
                const f32 d = 10.0f + static_cast<f32>(x) * 0.25f - static_cast<f32>(y);
                store.set(x, y, z, {voxel::quantize(d), d > 0.0f ? voxel::earth : voxel::air});
            }
        }
    }
    const voxel::Brush flatten{.mode = voxel::BrushMode::Flatten,
                               .center = {16.0f, 14.0f, 16.0f},
                               .radius = 8.0f,
                               .strength = 1.0f};
    for (int i = 0; i < 4; ++i) {
        static_cast<void>(voxel::apply(store, flatten));
    }
    // In der Mitte liegt die Oberfläche auf der Höhe 14, links wurde aufgefüllt, rechts
    // abgetragen.
    CHECK(store.distance(16, 14, 16) == 0.0f);
    CHECK(store.distance(13, 13, 16) > 0.0f);
    CHECK(store.distance(13, 13, 16) > 10.0f + 13.0f * 0.25f - 13.0f);
    CHECK(store.distance(19, 15, 16) < 0.0f);
    CHECK(store.sample(13, 13, 16).material == voxel::earth);
    // Außerhalb der Kugel bleibt alles.
    CHECK(store.distance(30, 10, 16) == voxel::dequantize(voxel::quantize(10.0f + 7.5f - 10.0f)));

    const voxel::Sample before_air = store.sample(16, 20, 16);
    const f32 before_density = store.distance(16, 12, 16);
    const voxel::SampleBox painted = voxel::apply(store, {.mode = voxel::BrushMode::Paint,
                                                          .center = {16.0f, 14.0f, 16.0f},
                                                          .radius = 4.0f,
                                                          .material = voxel::rock});
    CHECK_FALSE(painted.empty());
    CHECK(store.sample(16, 12, 16).material == voxel::rock);
    CHECK(store.distance(16, 12, 16) == before_density);
    CHECK(store.sample(16, 20, 16) == before_air);
    CHECK(store.sample(16, 4, 16).material == voxel::earth);
    // Noch einmal: nichts mehr zu tun.
    CHECK(voxel::apply(store, {.mode = voxel::BrushMode::Paint,
                               .center = {16.0f, 14.0f, 16.0f},
                               .radius = 4.0f,
                               .material = voxel::rock})
              .empty());
}

TEST_CASE("A crater survives unloading and shows in coarse levels while streaming", "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    voxel::TerrainDeltas deltas{test_world};
    voxel::Streamer streamer{test_world, test_stream};
    streamer.set_deltas(&deltas);
    MeshMap meshes;
    const voxel::Generator& generator = deltas.generator();
    const auto ground = static_cast<f32>(generator.height(0, 0));
    std::array observer{DVec3{0.0, ground + 10.0, 0.0}};
    settle(jobs, streamer, store, observer, meshes);
    REQUIRE(streamer.layout().leaf_of(0, static_cast<i64>(ground), 0)->level == 0);

    // Krater graben: Die Blätter der Stufe 0 ändern sich sofort.
    const voxel::TerrainEdit edit = deltas.apply(
        store, {.mode = voxel::BrushMode::Remove, .center = {0.0f, ground, 0.0f}, .radius = 12.0f});
    REQUIRE_FALSE(edit.empty());
    streamer.invalidate(edit.box);
    const voxel::ChunkCoord crater = voxel::chunk_of(0, static_cast<i64>(ground) - 6, 0);
    const voxel::Chunk dug = *store.find(crater);
    settle(jobs, streamer, store, observer, meshes);
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);

    // Wegfliegen, bis der Krater nur noch in groben Stufen liegt: Dort ist er auch.
    observer[0] = DVec3{1400.0, ground + 10.0, 0.0};
    settle(jobs, streamer, store, observer, meshes);
    CHECK(store.find(crater) == nullptr);
    const auto far_leaf = streamer.layout().leaf_of(0, static_cast<i64>(ground) - 6, 0);
    REQUIRE(far_leaf);
    CHECK(far_leaf->level >= 2);
    voxel::Chunk far_base;
    generator.fill(*far_leaf, far_base);
    CHECK_FALSE(same_samples(*store.find(*far_leaf), far_base));
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);

    // Zurück: Der Chunk entsteht neu, mit Krater.
    observer[0] = DVec3{0.0, ground + 10.0, 0.0};
    settle(jobs, streamer, store, observer, meshes);
    REQUIRE(store.find(crater) != nullptr);
    CHECK(same_samples(*store.find(crater), dug));
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
}

TEST_CASE("Edits refresh coarse leaves nearby without a new selection", "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    voxel::TerrainDeltas deltas{test_world};
    voxel::Streamer streamer{test_world, test_stream};
    streamer.set_deltas(&deltas);
    MeshMap meshes;
    const auto ground = static_cast<f32>(deltas.generator().height(0, 0));
    const std::array observer{DVec3{0.0, ground + 10.0, 0.0}};
    settle(jobs, streamer, store, observer, meshes);
    const u64 commits = streamer.stats().commits;

    // Ein Hügel 300 Voxel entfernt, wo grobe Blätter liegen: Der Pinsel rechnet ohne geladene
    // Chunks der Stufe 0, der Streamer erzeugt die groben Blätter dort neu.
    const auto hill_ground = static_cast<f32>(deltas.generator().height(300, 0));
    const voxel::TerrainEdit edit = deltas.apply(store, {.mode = voxel::BrushMode::Add,
                                                         .center = {300.0f, hill_ground, 0.0f},
                                                         .radius = 16.0f,
                                                         .material = voxel::sand});
    REQUIRE_FALSE(edit.empty());
    const auto leaf = streamer.layout().leaf_of(300, static_cast<i64>(hill_ground) + 8, 0);
    REQUIRE(leaf);
    REQUIRE(leaf->level > 0);
    const u64 before = voxel::checksum(*store.find(*leaf));
    streamer.invalidate(edit.box);
    settle(jobs, streamer, store, observer, meshes);
    CHECK(streamer.stats().commits > commits);
    CHECK(streamer.stats().refreshed_total > 0);
    CHECK(voxel::checksum(*store.find(*leaf)) != before);
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);

    // Undo: wieder wie vorher.
    streamer.invalidate(deltas.restore(store, edit.changes, false));
    settle(jobs, streamer, store, observer, meshes);
    CHECK(voxel::checksum(*store.find(*leaf)) == before);
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
}

TEST_CASE("Brushing while streaming in the background keeps chunks and deltas consistent",
          "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    voxel::TerrainDeltas deltas{test_world};
    voxel::StreamDesc desc = test_stream;
    desc.max_generating = 2;
    voxel::Streamer streamer{test_world, desc};
    streamer.set_deltas(&deltas);
    MeshMap meshes;
    // Dieselben Pinsel ohne Streaming: Jeder Chunk entsteht für die Rechnung aus Grundwelt und
    // Deltas. Rechnete ein Pinsel auf einem veralteten Chunk, gingen frühere Änderungen verloren.
    voxel::TerrainDeltas reference{test_world};
    voxel::ChunkStore reference_store;
    std::array observer{DVec3{0.0, 50.0, 0.0}};
    settle(jobs, streamer, store, observer, meshes);
    // Der Beobachter fliegt, in jedem Frame wird vor ihm gegraben, auch wo gerade Chunks
    // entstehen. Die Jobs brauchen Zeit, nicht Frames: Grenze nach Uhrzeit.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{10};
    for (int frame = 0; frame < 150 && std::chrono::steady_clock::now() < deadline; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        observer[0].x += 4.0;
        {
            const f32 x =
                static_cast<f32>(observer[0].x) + 30.0f + static_cast<f32>(frame % 9) * 12.0f;
            const auto ground = static_cast<f32>(deltas.generator().height(static_cast<i64>(x), 0));
            const voxel::Brush brush{
                .mode = voxel::BrushMode::Remove, .center = {x, ground, 0.0f}, .radius = 5.0f};
            streamer.invalidate(deltas.apply(store, brush).box);
            static_cast<void>(reference.apply(reference_store, brush));
        }
        apply(meshes, streamer.update(jobs, store, observer));
    }
    settle(jobs, streamer, store, observer, meshes);
    // Jeder Chunk im Store ist Grundwelt plus Deltas, und die Netze passen dazu.
    int wrong = 0;
    for (const voxel::ChunkCoord c : streamer.layout().leaves()) {
        voxel::Chunk expected;
        deltas.build(c, expected);
        wrong += same_samples(*store.find(c), expected) ? 0 : 1;
    }
    CHECK(wrong == 0);
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
    const voxel::DeltaMap& expected = *reference.snapshot();
    const voxel::DeltaMap& actual = *deltas.snapshot();
    CHECK(actual.size() == expected.size());
    int lost = 0;
    for (const auto& [coord, delta] : expected) {
        const auto it = actual.find(coord);
        lost += it != actual.end() && *it->second == *delta ? 0 : 1;
    }
    CHECK(lost == 0);
}

TEST_CASE("A brush on chunks that are being generated reaches them", "[voxel]") {
    jobs::JobSystem jobs;
    voxel::ChunkStore store;
    voxel::TerrainDeltas deltas{test_world};
    voxel::StreamDesc desc = test_stream;
    desc.max_generating = 1;
    voxel::Streamer streamer{test_world, desc};
    streamer.set_deltas(&deltas);
    MeshMap meshes;
    const auto ground = static_cast<f32>(deltas.generator().height(0, 0));
    std::array observer{DVec3{0.0, ground + 4.0, 0.0}};
    settle(jobs, streamer, store, observer, meshes);

    // Weit weg fliegen, in die Mitte eines Chunks der Stufe 0 an der Oberfläche: Er entsteht als
    // erster in einem Job.
    const auto far_ground = static_cast<f32>(deltas.generator().height(2992, 16));
    const voxel::ChunkCoord target = voxel::chunk_of(2992, static_cast<i64>(far_ground), 16);
    observer[0] = DVec3{2992.0, static_cast<f64>(voxel::chunk_origin(target.y)) + 16.0, 16.0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{10};
    while (streamer.stats().generating == 0 && std::chrono::steady_clock::now() < deadline) {
        apply(meshes, streamer.update(jobs, store, observer));
    }
    REQUIRE(streamer.stats().generating == 1);
    REQUIRE(store.find(target) == nullptr);

    // Jetzt dort graben: Der Job rechnet noch mit den alten Deltas.
    const voxel::TerrainEdit edit = deltas.apply(
        store,
        {.mode = voxel::BrushMode::Remove, .center = {2992.0f, far_ground, 16.0f}, .radius = 6.0f});
    REQUIRE_FALSE(edit.empty());
    streamer.invalidate(edit.box);
    settle(jobs, streamer, store, observer, meshes);
    REQUIRE(store.find(target) != nullptr);
    voxel::Chunk expected;
    deltas.build(target, expected);
    CHECK(same_samples(*store.find(target), expected));
    voxel::Chunk base;
    deltas.generator().fill(target, base);
    CHECK_FALSE(same_samples(*store.find(target), base));
    CHECK(stale_meshes(jobs, streamer, store, meshes) == 0);
}

TEST_CASE("Region files round-trip the deltas byte for byte", "[voxel]") {
    voxel::TerrainDeltas deltas{test_world};
    voxel::ChunkStore store;
    // Änderungen in mehreren Regionen, auch bei negativen Koordinaten.
    for (const f32 x : {-600.0f, -10.0f, 20.0f, 700.0f}) {
        const auto ground = static_cast<f32>(deltas.generator().height(static_cast<i64>(x), 5));
        static_cast<void>(deltas.apply(
            store,
            {.mode = voxel::BrushMode::Remove, .center = {x, ground, 5.0f}, .radius = 6.0f}));
    }
    const voxel::DeltaMap& original = *deltas.snapshot();
    REQUIRE(original.size() >= 4);
    const auto regions = voxel::encode_regions(original);
    CHECK(regions.size() >= 3);
    CHECK(voxel::encode_regions(original) == regions);

    voxel::DeltaMap loaded;
    usize count = 0;
    for (const auto& [region, bytes] : regions) {
        const std::string name = voxel::region_file_name(region);
        const auto parsed = voxel::parse_region_file_name(name);
        REQUIRE(parsed);
        CHECK(*parsed == region);
        const auto decoded = voxel::decode_region(region, bytes, loaded);
        REQUIRE(decoded);
        count += *decoded;
    }
    CHECK(count == original.size());
    REQUIRE(loaded.size() == original.size());
    for (const auto& [coord, delta] : original) {
        REQUIRE(loaded.contains(coord));
        CHECK(*loaded.at(coord) == *delta);
    }

    CHECK(voxel::region_file_name({-1, 0, 2}) == "r.-1.0.2.ndelta");
    CHECK_FALSE(voxel::parse_region_file_name("r.1.2.ndelta"));
    CHECK_FALSE(voxel::parse_region_file_name("r.1.2.x.ndelta"));
    CHECK_FALSE(voxel::parse_region_file_name("szene.json"));

    // Beschädigte Dateien ändern nichts.
    std::vector<std::byte> broken = regions.front().second;
    broken.resize(broken.size() - 3);
    voxel::DeltaMap untouched;
    CHECK_FALSE(voxel::decode_region(regions.front().first, broken, untouched));
    CHECK(untouched.empty());
    broken[0] = std::byte{'X'};
    CHECK_FALSE(voxel::decode_region(regions.front().first, broken, untouched));
}
