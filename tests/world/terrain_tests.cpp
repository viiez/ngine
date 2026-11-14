#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.reflect;
import ngine.voxel;
import ngine.world;

// Terrain als Komponente der Welt (R3.1): Zustand, Erzeugen, Meshing, Szenen. Seit R3.2 aus dem
// Generator mit Startwert.

using namespace ngine;

namespace {

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    return types;
}

} // namespace

TEST_CASE("update_terrains creates chunks and meshes them in jobs", "[world]") {
    const auto types = make_types();
    world::World world{types};
    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Entity terrain = world.create("Terrain");
    terrain.set(world::Terrain{.size_x = 2,
                               .size_y = 2,
                               .size_z = 3,
                               .shape = world::TerrainShape::Hills,
                               .ground = 10.0f});
    // Terrain bringt Transform mit, Transform die Weltlage.
    CHECK(terrain.has<world::Transform>());
    CHECK(terrain.has<world::WorldTransform>());

    const usize meshed = world::update_terrains(world, jobs);
    CHECK(meshed > 0);
    const auto* state = terrain.get<world::TerrainState>();
    REQUIRE(state != nullptr);
    REQUIRE(state->data != nullptr);
    const world::TerrainData& data = *state->data;
    CHECK(data.chunks.size() == 12);
    CHECK(data.stats.chunks == 12);
    CHECK(data.stats.meshes > 0);
    CHECK(data.stats.triangles > 0);
    // In x und z um die Position zentriert, von y = 0 aufwärts.
    CHECK(data.chunks.find({-1, 0, -1, 0}) != nullptr);
    CHECK(data.chunks.find({0, 1, 1, 0}) != nullptr);
    CHECK(data.chunks.find({1, 0, 0, 0}) == nullptr);
    // Oberfläche bei 10 m Höhe (20 Voxel) plus Hügel bis 6 m: unten fest, oben Luft.
    CHECK(data.chunks.sample(0, 2, 0).density > 0);
    CHECK(data.chunks.sample(0, 60, 0).density < 0);

    // Nichts geändert: nichts zu tun.
    CHECK(world::update_terrains(world, jobs) == 0);

    // Ein Pinsel über die Grenze von vier Chunks meshed nur die betroffenen neu.
    const u64 id = data.id;
    const auto hit =
        voxel::raycast(state->data->chunks, {0.0f, 100.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 200.0f);
    REQUIRE(hit);
    static_cast<void>(voxel::apply(state->data->chunks,
                                   {.mode = voxel::BrushMode::Remove, .center = hit->position}));
    const auto versions = [&] {
        u64 sum = 0;
        for (const auto& [coord, mesh] : data.meshes) {
            sum += mesh.version;
        }
        return sum;
    };
    const u64 before = versions();
    const usize remeshed = world::update_terrains(world, jobs);
    CHECK(remeshed >= 4);
    CHECK(remeshed < 12);
    CHECK(versions() > before);
    CHECK(terrain.get<world::TerrainState>()->data->id == id);

    // Eine geänderte Beschreibung erzeugt alles neu.
    terrain.set(world::Terrain{
        .size_x = 1, .size_y = 1, .size_z = 1, .shape = world::TerrainShape::Flat, .ground = 4.0f});
    CHECK(world::update_terrains(world, jobs) > 0);
    CHECK(data.chunks.size() == 1);
    CHECK(data.chunks.sample(5, 7, 5).density > 0);
    CHECK(data.chunks.sample(5, 9, 5).density < 0);
    for (const auto& [coord, mesh] : data.meshes) {
        CHECK_FALSE(mesh.mesh.indices.empty());
    }
}

TEST_CASE("Scenes store the terrain description but not its chunks", "[world]") {
    const auto types = make_types();
    std::string text;
    {
        world::World world{types};
        jobs::JobSystem jobs{{.worker_count = 0}};
        world.create("Terrain")
            .set(world::Transform{.position = {1.0, 2.0, 3.0}})
            .set(world::Terrain{.size_x = 3, .shape = world::TerrainShape::Flat, .height = 2.5f});
        world::update_terrains(world, jobs);
        text = world::save_scene(world);
    }
    CHECK(text.find("\"Terrain\"") != std::string::npos);
    CHECK(text.find("\"Flat\"") != std::string::npos);
    CHECK(text.find("TerrainState") == std::string::npos);

    world::World loaded{types};
    REQUIRE(world::load_scene(loaded, text));
    usize count = 0;
    loaded.each<const world::Terrain>([&](world::Entity entity, const world::Terrain& terrain) {
        ++count;
        CHECK(terrain.size_x == 3);
        CHECK(terrain.shape == world::TerrainShape::Flat);
        CHECK(terrain.height == 2.5f);
        CHECK_FALSE(entity.has<world::TerrainState>());
    });
    CHECK(count == 1);
    CHECK(world::save_scene(loaded) == text);
}

TEST_CASE("Procedural terrains come from the generator with their seed", "[world]") {
    const auto types = make_types();
    world::World world{types};
    jobs::JobSystem jobs{{.worker_count = 2}};
    const world::Terrain description{.size_x = 2, .size_y = 2, .size_z = 2, .seed = 99};
    CHECK(description.shape == world::TerrainShape::Procedural);
    world::Entity terrain = world.create("Terrain").set(description);
    world::update_terrains(world, jobs);
    const world::TerrainData& data = *terrain.get<world::TerrainState>()->data;
    CHECK(data.stats.generate_ms > 0.0);
    CHECK(data.stats.generate_chunk_ms > 0.0);

    // Dieselben Chunks wie direkt aus dem Generator.
    voxel::ChunkStore direct;
    world::generate_terrain(description, direct, jobs);
    CHECK(voxel::checksum(direct) == voxel::checksum(data.chunks));
    const voxel::GeneratorDesc desc = world::generator_desc(description);
    CHECK(desc.seed == 99);
    CHECK(desc.caves > 0.0f);

    // Anderer Startwert, andere Welt.
    world::Terrain other = description;
    other.seed = 100;
    voxel::ChunkStore different;
    world::generate_terrain(other, different, jobs);
    CHECK(voxel::checksum(different) != voxel::checksum(data.chunks));

    // Flach und Hügel schalten Felsen, Überhänge und Höhlen ab.
    other.shape = world::TerrainShape::Hills;
    CHECK(world::generator_desc(other).caves == 0.0f);
    CHECK(world::generator_desc(other).mountains == 0.0f);
    CHECK(world::generator_desc(other).hills == other.height);
    other.shape = world::TerrainShape::Flat;
    CHECK(world::generator_desc(other).hills == 0.0f);
}

TEST_CASE("Endless terrains stream around observers in levels", "[world]") {
    const auto types = make_types();
    world::World world{types};
    jobs::JobSystem jobs{{.worker_count = 2}};
    // Seit R3.3: endlos um die Beobachter, hier mit kurzer Sicht für einen schnellen Test.
    world::Entity terrain = world.create("Terrain").set(world::Terrain{
        .size_y = 4, .seed = 21, .streaming = true, .view_distance = 300.0f, .detail = 1.5f});
    world::Entity eye = world.create("Kamera")
                            .set(world::Transform{.position = {0.0, 30.0, 0.0}})
                            .set(world::TerrainObserver{});
    CHECK(eye.has<world::WorldTransform>());
    static_cast<void>(world::update_transforms(world));
    CHECK(world::update_terrains(world, jobs, {.wait = true}) > 0);

    const world::TerrainData& data = *terrain.get<world::TerrainState>()->data;
    REQUIRE(data.streamer);
    CHECK(data.stats.streaming);
    CHECK(data.stats.stream.leaves == data.streamer->layout().size());
    CHECK(data.stats.stream.per_level[0] > 0);
    CHECK(data.stats.stream.per_level[2] > 0);
    CHECK(data.chunks.size() == data.streamer->layout().size());
    CHECK(data.stats.meshes == data.meshes.size());
    CHECK(data.stats.triangles > 0);
    // Netze gibt es nur für Blätter der Auswahl, und nur nicht leere.
    for (const auto& [coord, mesh] : data.meshes) {
        CHECK(data.streamer->layout().contains(coord));
        CHECK_FALSE(mesh.mesh.indices.empty());
    }
    // Stufe des Blatts an einer Stelle (Voxel relativ zum Terrain); -1 außerhalb der Sicht.
    const auto level_at = [&](i64 x, i64 y, i64 z) {
        const auto leaf = data.streamer->layout().leaf_of(x, y, z);
        return leaf ? i32{leaf->level} : -1;
    };
    // Der Chunk unter dem Beobachter ist fein (Stufe 0).
    CHECK(level_at(0, 60, 0) == 0);

    // Beobachter 200 m weiter: Dort wird es fein, am alten Ort grob. Positionen relativ zum
    // Terrain (in Voxeln) folgen der Lage der Entity.
    eye.set(world::Transform{.position = {200.0, 30.0, 0.0}});
    static_cast<void>(world::update_transforms(world));
    CHECK(world::update_terrains(world, jobs, {.wait = true}) > 0);
    CHECK(level_at(400, 60, 0) == 0);
    CHECK(level_at(0, 60, 0) > 0);

    // Ein Beobachter ohne Entity (Kamera des Editors); der inaktive zählt nicht mehr.
    eye.set(world::TerrainObserver{.active = false});
    const math::DVec3 editor{-100.0, 30.0, 50.0};
    world::update_terrains(world, jobs, {.observers = std::span{&editor, 1}, .wait = true});
    CHECK(level_at(-200, 60, 100) == 0);
    CHECK(level_at(400, 60, 0) != 0);
}

TEST_CASE("Endless terrains without observers load around their position", "[world]") {
    const auto types = make_types();
    world::World world{types};
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Entity terrain = world.create("Terrain")
                                .set(world::Transform{.position = {1.0e6, 0.0, -2.0e6}})
                                .set(world::Terrain{.streaming = true, .view_distance = 200.0f});
    static_cast<void>(world::update_transforms(world));
    world::update_terrains(world, jobs, {.wait = true});
    const world::TerrainData& data = *terrain.get<world::TerrainState>()->data;
    REQUIRE(data.streamer);
    CHECK(data.streamer->layout().leaf_of(0, 40, 0)->level == 0);
    CHECK_FALSE(data.meshes.empty());

    // Zurück zu fester Größe: Streamer weg, Chunks wie bisher.
    world::Terrain fixed = *terrain.get<world::Terrain>();
    fixed.streaming = false;
    fixed.size_x = 2;
    fixed.size_z = 2;
    terrain.set(fixed);
    world::update_terrains(world, jobs);
    CHECK_FALSE(data.streamer);
    CHECK(data.chunks.size() == 2u * 2u * 4u);
}

TEST_CASE("Scenes store terrain observers and streaming settings", "[world]") {
    const auto types = make_types();
    world::World world{types};
    world.create("Terrain").set(
        world::Terrain{.streaming = true, .view_distance = 1500.0f, .detail = 2.5f});
    world.create("Kamera").set(world::TerrainObserver{});
    const std::string text = world::save_scene(world);
    CHECK(text.find("\"streaming\": true") != std::string::npos);
    CHECK(text.find("TerrainObserver") != std::string::npos);
    world::World loaded{types};
    REQUIRE(world::load_scene(loaded, text));
    CHECK(world::save_scene(loaded) == text);
}

TEST_CASE("Terrain material table round-trips and matches the asset file", "[world]") {
    const world::TerrainMaterials standard = world::standard_terrain_materials();
    REQUIRE(standard.materials.size() == 5);
    CHECK(standard.materials[voxel::grass].name == "Gras");
    CHECK(standard.materials[voxel::rock].footsteps == "fels");

    const auto parsed = world::parse_terrain_materials(world::write_terrain_materials(standard));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == standard);

    // Die Datei im Asset-Ordner passt bis auf das Aussehen aus Texturen zur eingebauten Tabelle.
    const auto file = world::load_terrain_materials_file(NGINE_ASSET_SOURCE_DIR
                                                         "/terrain/standard.terrain-materials");
    REQUIRE(file.has_value());
    REQUIRE(file->materials.size() == standard.materials.size());
    for (usize i = 0; i < standard.materials.size(); ++i) {
        const world::TerrainMaterial& a = file->materials[i];
        const world::TerrainMaterial& b = standard.materials[i];
        CHECK(a.name == b.name);
        CHECK(a.roughness == b.roughness);
        CHECK(a.scale == b.scale);
        CHECK(a.normal_scale == b.normal_scale);
        CHECK(a.friction == b.friction);
        CHECK(a.restitution == b.restitution);
        CHECK(a.footsteps == b.footsteps);
        // Gras, Fels, Erde und Sand haben Texturen.
        CHECK((a.albedo != 0) == (i != 0));
    }

    CHECK_FALSE(world::parse_terrain_materials("{}").has_value());
    CHECK_FALSE(
        world::parse_terrain_materials(R"({"format": "ngine.terrain-materials"})").has_value());
}
