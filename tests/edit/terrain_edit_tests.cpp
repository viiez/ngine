#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>

import ngine.core;
import ngine.filesystem;
import ngine.jobs;
import ngine.math;
import ngine.reflect;
import ngine.voxel;
import ngine.world;
import ngine.edit;

// Voxel-Werkzeuge des Editors (EDIT-08, R3.4): Pinselstriche als ein Schritt im Verlauf, Undo und
// Redo über Deltas, Spielen und Löschen behalten die Deltas, Speichern und Öffnen über
// Regionen-Dateien neben der Szene. Alles ohne Fenster und GPU.

using namespace ngine;
using math::DVec3;

namespace {

struct Fixture {
    reflect::TypeRegistry types = [] {
        reflect::TypeRegistry t;
        world::register_types(t);
        return t;
    }();
    world::World world{types};
    events::EventBus bus;
    edit::SceneDocument document{world, &bus};
    jobs::JobSystem jobs{{.worker_count = 2}};

    // Endloses Terrain mit Sicht 200 m um einen Beobachter über dem Ursprung.
    edit::EntityId add_terrain(bool streaming = true) {
        auto created = edit::create_entity(document, "Terrain");
        REQUIRE(document.execute(std::move(created.command)));
        id = created.id;
        terrain().set(world::Terrain{
            .size_x = 4, .size_z = 4, .seed = 3, .streaming = streaming, .view_distance = 200.0f});
        update();
        return created.id;
    }
    world::Entity terrain() {
        return world.entity(document.entity_of(id));
    }
    const world::TerrainData& data() {
        return *terrain().get<world::TerrainState>()->data;
    }
    void update() {
        const DVec3 observer{0.0, 20.0, 0.0};
        // Grobe Blätter, die nach Änderungen neu entstehen, kommen einen Aufruf später.
        for (int i = 0; i < 3; ++i) {
            (void)world::update_transforms(world);
            (void)world::update_terrains(world, jobs, {.observers = {&observer, 1}, .wait = true});
        }
    }
    DVec3 surface() {
        const auto hit =
            world::raycast_terrain(terrain(), {0.0, 80.0, 0.0}, {0.0f, -1.0f, 0.0f}, 200.0);
        REQUIRE(hit);
        return *hit;
    }
    // Summe der Prüfsummen aller Chunks: ändert sich mit jedem Sample.
    u64 samples() {
        u64 sum = 0;
        for (const voxel::ChunkCoord c : data().chunks.coords()) {
            sum += voxel::checksum(*data().chunks.find(c)) * (static_cast<u64>(c.level) + 1);
        }
        return sum;
    }

    edit::EntityId id = 0;
};

std::string temp_scene() {
    std::random_device random;
    const auto path = std::filesystem::temp_directory_path() /
                      ("ngine_terrain_" + std::to_string(random())) / "welt.json";
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace

TEST_CASE("A brush stroke over several frames is one undo step", "[edit][terrain]") {
    Fixture f;
    f.add_terrain();
    const DVec3 ground = f.surface();
    const u64 original = f.samples();
    const usize history = f.document.history().size();

    edit::TerrainBrushTool tool;
    tool.settings = {.mode = voxel::BrushMode::Remove, .radius = 2.0f};
    // Fünf Frames, der Pinsel wandert einen Meter.
    for (int i = 0; i < 5; ++i) {
        const auto changed =
            tool.stroke(f.document, f.id, ground + DVec3{0.25 * i, 0.0, 0.0}, 1.0f / 60.0f);
        REQUIRE(changed);
        CHECK(*changed);
    }
    tool.end_stroke();
    CHECK_FALSE(tool.stroking());
    f.update();
    CHECK(f.document.history().size() == history + 1);
    CHECK(f.document.history().undo_label() == "Abtragen");
    CHECK(f.document.dirty());
    CHECK(f.data().deltas.size() > 0);
    CHECK(f.data().stats.delta_chunks == f.data().deltas.size());
    CHECK(f.data().stats.delta_bytes > 0);
    // Der Strahl fällt jetzt tiefer.
    CHECK(f.surface().y < ground.y - 0.5);
    const u64 dug = f.samples();
    CHECK(dug != original);

    REQUIRE(f.document.undo());
    f.update();
    CHECK(f.data().deltas.size() == 0);
    CHECK(f.samples() == original);
    CHECK(f.surface().y == ground.y);

    REQUIRE(f.document.redo());
    f.update();
    CHECK(f.samples() == dug);

    // Ein neuer Strich ist ein neuer Schritt; Bemalen ändert nur Material.
    tool.settings = {.mode = voxel::BrushMode::Paint, .radius = 3.0f, .material = voxel::sand};
    REQUIRE(tool.stroke(f.document, f.id, ground + DVec3{4.0, 0.0, 0.0}, 0.016f));
    tool.end_stroke();
    CHECK(f.document.history().size() == history + 2);
    CHECK(f.document.history().undo_label() == "Bemalen");
}

TEST_CASE("Terrain tools work on fixed terrains and refuse while playing", "[edit][terrain]") {
    Fixture f;
    f.add_terrain(false);
    const DVec3 ground = f.surface();
    edit::TerrainBrushTool tool;
    tool.settings = {.mode = voxel::BrushMode::Add, .radius = 1.5f, .material = voxel::sand};
    REQUIRE(tool.stroke(f.document, f.id, ground, 0.016f));
    tool.end_stroke();
    f.update();
    CHECK(f.surface().y > ground.y + 0.5);
    const u64 raised = f.samples();

    // Ebnen auf die Höhe des ersten Treffers trägt den Hügel wieder ab.
    tool.settings = {.mode = voxel::BrushMode::Flatten, .radius = 3.0f, .strength = 1.0f};
    for (int i = 0; i < 10; ++i) {
        REQUIRE(tool.stroke(f.document, f.id, ground + DVec3{0.0, 0.0, -1.5}, 0.1f));
    }
    tool.end_stroke();
    f.update();
    CHECK(f.samples() != raised);
    CHECK(f.surface().y < ground.y + 0.3);

    REQUIRE(f.document.begin_play());
    CHECK_FALSE(tool.stroke(f.document, f.id, ground, 0.016f));
    REQUIRE(f.document.end_play());
    CHECK_FALSE(tool.stroke(f.document, 999, ground, 0.016f));
}

TEST_CASE("Play, delete and undo keep the deltas of a terrain", "[edit][terrain]") {
    Fixture f;
    f.add_terrain();
    const DVec3 ground = f.surface();
    edit::TerrainBrushTool tool;
    tool.settings = {.mode = voxel::BrushMode::Remove, .radius = 2.0f};
    REQUIRE(tool.stroke(f.document, f.id, ground, 0.016f));
    tool.end_stroke();
    f.update();
    const auto deltas = world::terrain_deltas(f.terrain());
    REQUIRE(deltas);
    const u64 dug = f.samples();

    // Spielen: Was beim Spielen gegraben wird, ist danach weg, die Änderung davor bleibt.
    REQUIRE(f.document.begin_play());
    static_cast<void>(world::edit_terrain(
        f.terrain(),
        {.mode = voxel::BrushMode::Add,
         .center = {120.0f, static_cast<f32>(ground.y / voxel::voxel_size) + 30.0f, 120.0f},
         .radius = 6.0f}));
    CHECK(world::terrain_deltas(f.terrain())->size() > deltas->size());
    REQUIRE(f.document.end_play());
    f.update();
    CHECK(world::terrain_deltas(f.terrain())->size() == deltas->size());
    CHECK(f.samples() == dug);

    // Löschen und Undo: Das Terrain kommt mit seinen Deltas zurück; der Pinsel ist rückgängig
    // zu machen.
    REQUIRE(f.document.execute(edit::destroy_entity(f.id)));
    CHECK_FALSE(f.document.exists(f.id));
    REQUIRE(f.document.undo());
    f.update();
    CHECK(f.samples() == dug);
    REQUIRE(f.document.undo());
    f.update();
    CHECK(world::terrain_deltas(f.terrain())->empty());
}

TEST_CASE("Saving a scene writes region files and opening restores the changes",
          "[edit][terrain]") {
    const std::string path = temp_scene();
    u64 dug = 0;
    std::string folder;
    {
        Fixture f;
        f.add_terrain();
        const DVec3 ground = f.surface();
        edit::TerrainBrushTool tool;
        tool.settings = {.mode = voxel::BrushMode::Remove, .radius = 3.0f};
        REQUIRE(tool.stroke(f.document, f.id, ground, 0.016f));
        tool.end_stroke();
        // Zweite Änderung weit weg in einer anderen Region.
        static_cast<void>(world::edit_terrain(
            f.terrain(),
            {.mode = voxel::BrushMode::Add, .center = {-300.0f, 40.0f, 0.0f}, .radius = 4.0f}));
        f.update();
        dug = f.samples();

        REQUIRE(filesystem::create_directories(filesystem::parent_path(path)));
        REQUIRE(f.document.save(path));
        CHECK_FALSE(f.document.dirty());
        // Der Ordner heißt wie die Szene und steht im Terrain.
        folder = f.terrain().get<world::Terrain>()->deltas;
        CHECK(folder == "welt.terrain");
        const std::string dir = filesystem::join(filesystem::parent_path(path), folder);
        const auto files = filesystem::list_directory(dir);
        REQUIRE(files);
        CHECK(files->size() >= 2);
        for (const filesystem::FileEntry& file : *files) {
            CHECK(voxel::parse_region_file_name(file.path).has_value());
        }
    }
    {
        Fixture f;
        REQUIRE(f.document.open(path));
        f.world.each<const world::Terrain>([&](world::Entity entity, const world::Terrain&) {
            f.id = f.document.id_of(entity.id());
        });
        REQUIRE(f.id != 0);
        f.update();
        CHECK(f.data().deltas.size() > 0);
        CHECK(f.samples() == dug);

        // Alles rückgängig machen und speichern: Die Regionen-Dateien verschwinden.
        world::set_terrain_deltas(f.terrain(), nullptr);
        REQUIRE(f.document.save(path));
        const auto files =
            filesystem::list_directory(filesystem::join(filesystem::parent_path(path), folder));
        REQUIRE(files);
        CHECK(files->empty());
    }
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::path{path}.parent_path(), ignored);
}
