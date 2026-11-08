#include <catch2/catch_test_macros.hpp>

#include <string>

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.reflect;
import ngine.voxel;
import ngine.world;

// Terrain als Komponente der Welt (R3.1): Zustand, Erzeugen, Meshing, Szenen.

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
    terrain.set(world::Terrain{.size_x = 2, .size_y = 2, .size_z = 3});
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
