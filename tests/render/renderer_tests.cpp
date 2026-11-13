#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.jobs;
import ngine.asset;
import ngine.world;
import ngine.render;
import ngine.cook;
import ngine.filesystem;

// Renderer: Kamera ohne GPU, dann eine PBR-Kugel mit Tone Mapping über lavapipe.

using namespace ngine;
using Catch::Approx;

namespace {

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    return types;
}

math::Vec4 project(const math::Mat4& m, math::Vec3 p) {
    return m * math::Vec4{p.x, p.y, p.z, 1.0f};
}

bool gpu_required() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* value = std::getenv("NGINE_REQUIRE_GPU");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value != nullptr && *value != '\0';
}

// Meldet ein Mesh aus dem MeshStore als erzeugtes Modell beim Ressourcen-Manager an, damit ein
// MeshRenderer darauf verweisen kann (R2.4). Das Modell gibt das Mesh beim Zerstören frei.
asset::AssetRef as_asset(render::Renderer& r, asset::ResourceManager& resources, u32 mesh,
                         std::string_view name) {
    const asset::AssetId id = asset::generated_id(name);
    render::ModelInstance instance;
    instance.meshes.push_back(mesh);
    resources.add(id, render::GpuModel{r, std::move(instance), {}});
    r.set_resources(&resources);
    return {.asset = id, .index = 0};
}

} // namespace

TEST_CASE("Renderer uses the first active camera", "[render]") {
    const auto types = make_types();
    world::World world{types};
    CHECK_FALSE(render::find_camera(world));

    world.create("Aus").set(world::Transform{}).set(render::Camera{.active = false});
    // 10 000 km vom Ursprung, um 90° nach links gedreht: blickt entlang -x.
    const math::DVec3 far{1.0e7, 0.0, 0.0};
    world.create("Kamera")
        .set(world::Transform{.position = far,
                              .rotation = math::angle_axis(math::radians(90.0f), {0, 1, 0})})
        .set(render::Camera{.near_plane = 0.5f, .exposure = 2.0f});
    world::update_transforms(world);

    const auto camera = render::find_camera(world);
    REQUIRE(camera);
    CHECK(camera->position == far);
    CHECK(camera->camera.exposure == 2.0f);

    const math::Mat4 vp = render::view_projection(*camera, 2.0f);
    // Ein Punkt 4 m vor der Kamera (entlang -x) landet in der Bildmitte.
    const math::Vec4 ahead = project(vp, {-4.0f, 0.0f, 0.0f});
    CHECK(ahead.x / ahead.w == Approx(0.0f).margin(1e-5f));
    CHECK(ahead.y / ahead.w == Approx(0.0f).margin(1e-5f));
    // Umgekehrte Tiefe: nahe Ebene 1, weiter weg kleiner, nie unter 0.
    CHECK(ahead.z / ahead.w == Approx(0.5f / 4.0f));
    const math::Vec4 near_point = project(vp, {-0.5f, 0.0f, 0.0f});
    CHECK(near_point.z / near_point.w == Approx(1.0f));
    // Rechts der Kamera liegt nach der Drehung -z der Welt.
    CHECK(project(vp, {-4.0f, 0.0f, -1.0f}).x > 0.0f);
    CHECK(project(vp, {-4.0f, 1.0f, 0.0f}).y > 0.0f);
    // Hinter der Kamera wird w negativ.
    CHECK(project(vp, {4.0f, 0.0f, 0.0f}).w < 0.0f);
}

TEST_CASE("MeshRenderer is saved with scenes as asset references", "[render]") {
    const auto types = make_types();
    CHECK_FALSE(types.get<render::MeshRenderer>().transient);
    CHECK_FALSE(types.get<render::Camera>().transient);

    world::World world{types};
    world.create("Kiste")
        .set(world::Transform{})
        .set(render::MeshRenderer{.mesh = {.asset = 1234567890123ull, .index = 2},
                                  .material = {.asset = 99, .index = 1}});
    const std::string text = world::save_scene(world);
    world::World loaded{types};
    REQUIRE(world::load_scene(loaded, text));
    bool found = false;
    loaded.each<const render::MeshRenderer>([&](const render::MeshRenderer& renderer) {
        found = true;
        CHECK(renderer.mesh.asset == 1234567890123ull);
        CHECK(renderer.mesh.index == 2);
        CHECK(renderer.material.asset == 99);
        CHECK(renderer.material.index == 1);
    });
    CHECK(found);
    CHECK(world::save_scene(loaded) == text);
}

TEST_CASE("Debug texts are collected per frame and cleared with the lines", "[render]") {
    render::DebugDraw debug;
    debug.text(math::DVec3{1.0, 2.0, 3.0}, "Licht 1", {1.0f, 0.5f, 0.0f});
    debug.line(math::DVec3{}, math::DVec3{1.0, 0.0, 0.0}, {1.0f, 1.0f, 1.0f});
    REQUIRE(debug.texts().size() == 1);
    CHECK(debug.texts()[0].text == "Licht 1");
    CHECK(debug.texts()[0].position.z == 3.0);
    debug.clear();
    CHECK(debug.texts().empty());
    CHECK(debug.lines().empty());
    debug.enabled = false;
    debug.text(math::DVec3{}, "aus");
    CHECK(debug.texts().empty());
}

TEST_CASE("Renderer draws a PBR sphere with tone mapping", "[render][gpu]") {
    auto created =
        rhi::vulkan::create_device({.application_name = "NGine Tests", .enable_validation = true});
    if (!created) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + created.error());
        }
        SKIP("Kein nutzbares Vulkan-Gerät: " + created.error());
    }
    const std::unique_ptr<rhi::Device> device = *std::move(created);
    constexpr u32 size = 64;

    auto target = device->create_texture(
        {.extent = {size, size},
         .format = rhi::Format::RGBA8Unorm,
         .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc});
    REQUIRE(target);
    auto renderer = render::Renderer::create(*device, rhi::Format::RGBA8Unorm);
    REQUIRE(renderer);
    render::Renderer& r = **renderer;
    // Kaum Umgebungslicht; die Sonne (unten) kommt von rechts, die linke Seite bleibt dunkel.
    r.lighting = {.sky_color = {0.02f, 0.02f, 0.02f}, .ground_color = {0.02f, 0.02f, 0.02f}};

    const asset::Primitive sphere = asset::make_sphere(1.0f, 32, 16);
    const u32 material = r.materials().add(
        {.base_color = {0.8f, 0.2f, 0.2f, 1.0f}, .metallic = 0.0f, .roughness = 0.6f});
    const std::vector<render::SubmeshData> submeshes{
        {.vertices = sphere.vertices, .indices = sphere.indices, .material = material}};
    const auto mesh = r.meshes().add(submeshes);
    REQUIRE(mesh);
    asset::ResourceManager resources;
    const asset::AssetRef sphere_ref = as_asset(r, resources, *mesh, "test.kugel");

    const auto types = make_types();
    world::World world{types};
    // Weit vom Ursprung: kamerarelatives Zeichnen hält die Kugel trotzdem genau in der Mitte.
    const math::DVec3 origin{5.0e6, 2.0e3, -3.0e6};
    world.create("Kugel")
        .set(world::Transform{.position = origin})
        .set(render::MeshRenderer{.mesh = sphere_ref});
    world.create("Kamera")
        .set(world::Transform{.position = origin + math::DVec3{0.0, 0.0, 3.0}})
        .set(render::Camera{.fov_y = math::radians(45.0f)});
    // Leuchtet entlang -x, also von rechts.
    world.create("Sonne")
        .set(world::Transform{.rotation = math::look_rotation({-1.0f, 0.0f, 0.0f}, {0, 1, 0})})
        .set(render::DirectionalLight{.intensity = 3.0f});
    world::update_transforms(world);
    r.shadows.resolution = 256;

    render::TexturePool pool{*device};
    for (int frame = 0; frame < 2; ++frame) {
        render::RenderGraph graph;
        const auto output = graph.import_texture("Ziel", **target, rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::TransferSrc);
        r.add_passes(graph, world, output);
        device->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
        pool.end_frame();
    }
    CHECK(r.stats().draws == 1);
    // Ein undurchsichtiges Netz: ein Draw-Aufruf je Phase.
    CHECK(r.stats().forward_draws == 2);
    CHECK(r.stats().triangles == sphere.indices.size() / 3);

    const auto pixels = device->read_texture(**target, rhi::ResourceState::TransferSrc);
    const auto pixel = [&](u32 x, u32 y) {
        const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
        return std::vector<int>{pixels[i], pixels[i + 1], pixels[i + 2]};
    };
    const auto lit = pixel(44, 32);
    const auto shadow = pixel(20, 32);
    const auto sky = pixel(2, 2);
    INFO("Licht " << lit[0] << "," << lit[1] << "," << lit[2] << " Schatten " << shadow[0] << ","
                  << shadow[1] << "," << shadow[2] << " Himmel " << sky[0]);
    // Lichtseite rot und hell, Schattenseite dunkel, Himmel fast schwarz.
    CHECK(lit[0] > 100);
    CHECK(lit[0] > lit[1] + 50);
    CHECK(lit[0] > shadow[0] + 80);
    CHECK(shadow[0] < 60);
    CHECK(sky[0] < 40);
    CHECK(sky[0] == sky[2]);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Renderer picks the entity under a pixel from the ID buffer", "[render][gpu]") {
    auto created =
        rhi::vulkan::create_device({.application_name = "NGine Tests", .enable_validation = true});
    if (!created) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + created.error());
        }
        SKIP("Kein nutzbares Vulkan-Gerät: " + created.error());
    }
    const std::unique_ptr<rhi::Device> device = *std::move(created);
    constexpr u32 size = 64;
    auto target = device->create_texture(
        {.extent = {size, size},
         .format = rhi::Format::RGBA8Unorm,
         .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc});
    REQUIRE(target);
    auto renderer = render::Renderer::create(*device, rhi::Format::RGBA8Unorm);
    REQUIRE(renderer);
    render::Renderer& r = **renderer;

    const asset::Primitive box = asset::make_box({0.5f, 0.5f, 0.5f});
    const std::vector<render::SubmeshData> submeshes{
        {.vertices = box.vertices, .indices = box.indices, .material = 0}};
    const auto mesh = r.meshes().add(submeshes);
    REQUIRE(mesh);
    asset::ResourceManager resources;
    const asset::AssetRef box_ref = as_asset(r, resources, *mesh, "test.wuerfel");

    const auto types = make_types();
    world::World world{types};
    // Zwei Würfel nebeneinander, der rechte steht vor einem dritten, der verdeckt bleibt.
    const world::Entity left = world.create("Links")
                                   .set(world::Transform{.position = {-1.0, 0.0, 0.0}})
                                   .set(render::MeshRenderer{.mesh = box_ref});
    const world::Entity right = world.create("Rechts")
                                    .set(world::Transform{.position = {1.0, 0.0, 0.0}})
                                    .set(render::MeshRenderer{.mesh = box_ref});
    world.create("Verdeckt")
        .set(world::Transform{.position = {1.0, 0.0, -3.0}})
        .set(render::MeshRenderer{.mesh = box_ref});
    world.create("Kamera")
        .set(world::Transform{.position = {0.0, 0.0, 5.0}})
        .set(render::Camera{.fov_y = math::radians(45.0f)});
    world::update_transforms(world);

    render::TexturePool pool{*device};
    const auto pick = [&](u32 x, u32 y) -> std::optional<render::PickResult> {
        r.request_pick(x, y);
        // Das Ergebnis kommt, wenn derselbe Platz wieder an der Reihe ist.
        for (u32 frame = 0; frame < device->frames_in_flight() + 1; ++frame) {
            render::RenderGraph graph;
            const auto output = graph.import_texture(
                "Ziel", **target, rhi::ResourceState::Undefined, rhi::ResourceState::TransferSrc);
            r.add_passes(graph, world, output);
            device->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
            pool.end_frame();
            if (auto result = r.take_pick(world)) {
                return result;
            }
        }
        return std::nullopt;
    };

    // Bildmitte des linken bzw. rechten Würfels: x = 32 -+ 1 / tan(22,5°) / 5 * 32.
    const auto on_left = pick(14, 32);
    REQUIRE(on_left);
    CHECK(on_left->x == 14);
    CHECK(on_left->entity == left.id());
    const auto on_right = pick(50, 32);
    REQUIRE(on_right);
    CHECK(on_right->entity == right.id());
    const auto sky = pick(32, 2);
    REQUIRE(sky);
    CHECK(sky->entity == 0);
    // Außerhalb des Ziels: sofort nichts.
    const auto outside = pick(size + 5, 0);
    REQUIRE(outside);
    CHECK(outside->entity == 0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Renderer loads glTF models by asset id in the background", "[render][asset][gpu]") {
    auto created =
        rhi::vulkan::create_device({.application_name = "NGine Tests", .enable_validation = true});
    if (!created) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + created.error());
        }
        SKIP("Kein nutzbares Vulkan-Gerät: " + created.error());
    }
    const std::unique_ptr<rhi::Device> device = *std::move(created);
    constexpr u32 size = 32;
    auto target = device->create_texture(
        {.extent = {size, size},
         .format = rhi::Format::RGBA8Unorm,
         .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc});
    REQUIRE(target);
    auto renderer = render::Renderer::create(*device, rhi::Format::RGBA8Unorm);
    REQUIRE(renderer);
    render::Renderer& r = **renderer;
    r.shadows.enabled = false;

    // Die Beispiel-Assets des Repositorys samt ihrer .meta-Dateien.
    auto database = asset::AssetDatabase::open(NGINE_ASSET_SOURCE_DIR);
    REQUIRE(database);
    const asset::AssetInfo* kiste = database->find_path("modelle/kiste.gltf");
    REQUIRE(kiste != nullptr);

    jobs::JobSystem jobs{{.worker_count = 1}};
    asset::ResourceManager resources{&*database, &jobs};
    render::register_loaders(resources, r);
    r.set_resources(&resources);

    const auto types = make_types();
    world::World world{types};
    world.create("Kiste")
        .set(world::Transform{})
        .set(render::MeshRenderer{.mesh = {.asset = kiste->id, .index = 0}});
    world.create("Kamera")
        .set(world::Transform{.position = {0.0, 0.0, 3.0}})
        .set(render::Camera{.fov_y = math::radians(45.0f)});
    world.create("Sonne")
        .set(world::Transform{.rotation = math::look_rotation({0.0f, -0.5f, -1.0f}, {0, 1, 0})})
        .set(render::DirectionalLight{.intensity = 3.0f});
    world::update_transforms(world);

    render::TexturePool pool{*device};
    const auto frame = [&] {
        resources.update();
        render::RenderGraph graph;
        const auto output = graph.import_texture("Ziel", **target, rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::TransferSrc);
        r.add_passes(graph, world, output);
        device->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
        pool.end_frame();
    };

    // Erster Frame: Das Modell lädt noch und wird nicht gezeichnet.
    frame();
    CHECK(r.stats().draws == 0);
    CHECK(r.stats().waiting == 1);

    resources.wait();
    for (int i = 0; i < 4 && r.stats().draws == 0; ++i) {
        frame();
    }
    REQUIRE(r.stats().draws == 1);
    CHECK(r.stats().waiting == 0);
    const render::GpuModel* model = r.find_model(kiste->id);
    REQUIRE(model != nullptr);
    CHECK(model->instance().textures.size() == 1);
    // Der Aufbau bleibt, die großen Daten liegen nur auf der GPU.
    CHECK(model->layout().meshes.size() == 1);
    CHECK(model->layout().meshes[0].primitives[0].vertices.empty());
    CHECK(model->layout().images[0].pixels.empty());

    const auto pixels = device->read_texture(**target, rhi::ResourceState::TransferSrc);
    const std::size_t center = (static_cast<std::size_t>(size / 2) * size + size / 2) * 4;
    const std::size_t corner = 0;
    INFO("Mitte " << int{pixels[center]} << ", Ecke " << int{pixels[corner]});
    CHECK(pixels[center] != pixels[corner]);
    CHECK(resources.stats().resident == 1);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Renderer loads cooked models from the cache with and without BC7",
          "[render][asset][gpu]") {
    // Erst kocht der Lader (wie im Editor) und legt das Modell in den Cache, dann liest ein
    // zweiter Ressourcen-Manager es gekocht. Mit BC7 und mit dem Rückfall auf RGBA8.
    std::random_device random;
    const auto base =
        std::filesystem::temp_directory_path() / ("ngine_cache_" + std::to_string(random()));
    const std::u8string base_text = base.u8string();
    const std::string cache_dir{base_text.begin(), base_text.end()};
    const asset::AssetCache cache{cache_dir};
    auto database = asset::AssetDatabase::open(NGINE_ASSET_SOURCE_DIR);
    REQUIRE(database);
    const asset::AssetInfo* kiste = database->find_path("modelle/kiste.gltf");
    REQUIRE(kiste != nullptr);

    for (const bool bc : {true, false}) {
        INFO("BC7 erlaubt: " << bc);
        auto created = rhi::vulkan::create_device(
            {.application_name = "NGine Tests", .enable_validation = true, .bc_compression = bc});
        if (!created) {
            if (gpu_required()) {
                FAIL("Kein nutzbares Vulkan-Gerät: " + created.error());
            }
            SKIP("Kein nutzbares Vulkan-Gerät: " + created.error());
        }
        const std::unique_ptr<rhi::Device> device = *std::move(created);
        if (!bc) {
            CHECK_FALSE(device->capabilities().bc_compression);
        }
        auto renderer = render::Renderer::create(*device, rhi::Format::RGBA8Unorm);
        REQUIRE(renderer);
        render::Renderer& r = **renderer;

        for (const bool first_load : {true, false}) {
            // Nur ganz am Anfang ist der Cache leer.
            const bool first_round = bc && first_load;
            jobs::JobSystem jobs{{.worker_count = 1}};
            asset::ResourceManager resources{&*database, &jobs};
            render::register_loaders(
                resources, r,
                {.cache = &cache, .cook = cook::model_cooker({.textures = {.uastc_level = 0}})});
            const render::ModelLoadCounts before = render::model_load_counts();
            auto handle = resources.load<render::GpuModel>(kiste->id);
            for (int i = 0; i < 200 && handle.get() == nullptr; ++i) {
                resources.wait();
                device->execute([](rhi::CommandList&) {});
                resources.update();
            }
            REQUIRE(handle.get() != nullptr);
            const render::ModelLoadCounts after = render::model_load_counts();
            if (first_round) {
                CHECK(after.cooked == before.cooked + 1);
            } else {
                CHECK(after.cached == before.cached + 1);
            }
            CHECK(after.source == before.source);
            CHECK(handle.get()->instance().textures.size() == 1);
            CHECK(cache.model_current(kiste->id));
        }
        CHECK(device->validation_errors() == 0);
    }
    static_cast<void>(filesystem::remove(cache_dir));
}

TEST_CASE("Renderer culls meshlets hidden inside a cave", "[render][gpu]") {
    // Die Kamera steht in einer geschlossenen Kiste (doppelseitig, also von innen sichtbar). Vor
    // ihr, hinter der Wand, liegt eine Kugel im Stumpf: Das Hi-Z des letzten Frames verwirft sie,
    // und die zweite Phase bestätigt es (R3.5). Mit und ohne Mesh Shader.
    const bool baseline = GENERATE(false, true);
    auto created = rhi::vulkan::create_device({.application_name = "NGine Tests",
                                               .enable_validation = true,
                                               .disabled_features = {.mesh_shader = baseline}});
    if (!created) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + created.error());
        }
        SKIP("Kein nutzbares Vulkan-Gerät: " + created.error());
    }
    const std::unique_ptr<rhi::Device> device = *std::move(created);
    constexpr u32 size = 64;
    auto target = device->create_texture(
        {.extent = {size, size},
         .format = rhi::Format::RGBA8Unorm,
         .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc});
    REQUIRE(target);
    auto renderer = render::Renderer::create(*device, rhi::Format::RGBA8Unorm);
    REQUIRE(renderer);
    render::Renderer& r = **renderer;

    const asset::Primitive box = asset::make_box({5.0f, 5.0f, 5.0f});
    const asset::Primitive sphere = asset::make_sphere(2.0f, 64, 32);
    const u32 wall =
        r.materials().add({.base_color = {0.5f, 0.5f, 0.5f, 1.0f}, .double_sided = true});
    const u32 red = r.materials().add({.base_color = {0.8f, 0.1f, 0.1f, 1.0f}});
    const std::vector<render::SubmeshData> box_data{
        {.vertices = box.vertices, .indices = box.indices, .material = wall}};
    const std::vector<render::SubmeshData> sphere_data{
        {.vertices = sphere.vertices, .indices = sphere.indices, .material = red}};
    const auto box_mesh = r.meshes().add(box_data);
    const auto sphere_mesh = r.meshes().add(sphere_data);
    REQUIRE(box_mesh);
    REQUIRE(sphere_mesh);
    asset::ResourceManager resources;
    const asset::AssetRef box_ref = as_asset(r, resources, *box_mesh, "test.hoehle");
    const asset::AssetRef sphere_ref = as_asset(r, resources, *sphere_mesh, "test.kugel");

    const auto types = make_types();
    world::World world{types};
    world.create("Höhle").set(world::Transform{}).set(render::MeshRenderer{.mesh = box_ref});
    world.create("Kugel")
        .set(world::Transform{.position = {0.0, 0.0, -20.0}})
        .set(render::MeshRenderer{.mesh = sphere_ref});
    world.create("Kamera")
        .set(world::Transform{.position = {0.0, 0.0, 2.0}})
        .set(render::Camera{.fov_y = math::radians(60.0f)});
    world::update_transforms(world);

    render::TexturePool pool{*device};
    // Statistik kommt einige Frames später zurück (wie die Auswahl).
    for (u32 frame = 0; frame < device->frames_in_flight() + 4; ++frame) {
        render::RenderGraph graph;
        const auto output = graph.import_texture("Ziel", **target, rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::TransferSrc);
        r.add_passes(graph, world, output);
        device->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
        pool.end_frame();
    }
    const render::RenderStats& stats = r.stats();
    INFO("Meshlets " << stats.meshlets << ", gezeichnet " << stats.meshlets_drawn << ", Stumpf "
                     << stats.culled_frustum << ", Rückseiten " << stats.culled_backface
                     << ", verdeckt " << stats.culled_occluded);
    CHECK(stats.mesh_shaders == (!baseline && device->capabilities().features.mesh_shader));
    CHECK(stats.draws == 2);
    CHECK(stats.meshlets > 4);
    CHECK(stats.meshlets_drawn > 0);
    // Alle Meshlets der Kugel liegen hinter der Wand.
    CHECK(stats.culled_occluded > 0);
    CHECK(stats.meshlets_drawn < stats.meshlets);

    // Ohne Prüfung auf Verdeckung zeichnet die GPU die Kugel wieder.
    r.occlusion_culling = false;
    for (u32 frame = 0; frame < device->frames_in_flight() + 2; ++frame) {
        render::RenderGraph graph;
        const auto output = graph.import_texture("Ziel", **target, rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::TransferSrc);
        r.add_passes(graph, world, output);
        device->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
        pool.end_frame();
    }
    CHECK(r.stats().culled_occluded == 0);
    CHECK(device->validation_errors() == 0);
}
