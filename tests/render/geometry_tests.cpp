#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.reflect;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.voxel;
import ngine.world;
import ngine.render;

// Gemeinsame Geometriepuffer (TD-23) und Terrain im Renderer (R3.1).

using namespace ngine;

namespace {

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

} // namespace

TEST_CASE("RangeAllocator reuses and merges free ranges", "[render]") {
    render::RangeAllocator ranges{1024};
    const auto a = ranges.allocate(100, 64);
    const auto b = ranges.allocate(200, 64);
    const auto c = ranges.allocate(300, 64);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(*a == 0);
    CHECK(*b == 128);
    CHECK(*c == 384);
    CHECK(ranges.used() == 600);
    // Zu groß für den Rest.
    CHECK_FALSE(ranges.allocate(400, 64));

    // Die Lücke von b wird wiederverwendet.
    ranges.free(*b, 200);
    const auto d = ranges.allocate(150, 64);
    REQUIRE(d);
    CHECK(*d == 128);
    ranges.free(*d, 150);

    // Alles frei: ein einziger Bereich, der ganze Speicher am Stück.
    ranges.free(*a, 100);
    ranges.free(*c, 300);
    CHECK(ranges.used() == 0);
    CHECK(ranges.free_ranges() == 1);
    CHECK(ranges.largest_free() == 1024);
    CHECK(ranges.allocate(1024) == u64{0});
}

TEST_CASE("Renderer draws terrain chunks from shared geometry buffers", "[render][gpu]") {
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
    r.shadows.resolution = 256;

    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    world::World world{types};
    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Entity terrain = world.create("Terrain");
    terrain.set(world::Terrain{.size_x = 2,
                               .size_y = 1,
                               .size_z = 2,
                               .shape = world::TerrainShape::Hills,
                               .ground = 8.0f});
    world.create("Kamera")
        .set(world::Transform{.position = {0.0, 30.0, 30.0},
                              .rotation = math::look_rotation(
                                  math::normalize(math::Vec3{0.0f, -1.0f, -1.0f}), {0, 1, 0})})
        .set(render::Camera{});
    world.create("Sonne")
        .set(world::Transform{.rotation = math::look_rotation(
                                  math::normalize(math::Vec3{0.3f, -1.0f, -0.2f}), {0, 1, 0})})
        .set(render::DirectionalLight{});

    render::TexturePool pool{*device};
    const auto frame = [&] {
        world::update_terrains(world, jobs);
        world::update_transforms(world);
        render::RenderGraph graph;
        const auto output = graph.import_texture("Ziel", **target, rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::TransferSrc);
        r.add_passes(graph, world, output);
        device->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
        pool.end_frame();
    };
    frame();
    const world::TerrainData& data = *terrain.get<world::TerrainState>()->data;
    REQUIRE(data.meshes.size() > 0);
    CHECK(r.stats().terrain_chunks == data.meshes.size());
    CHECK(r.stats().terrain_uploads == data.meshes.size());
    CHECK(r.stats().triangles == data.stats.triangles);
    CHECK(r.stats().shadow_draws > 0);
    // Alle Netze in einem Block.
    CHECK(r.geometry().stats().blocks == 1);
    const u64 used = r.geometry().stats().used;
    CHECK(used > 0);

    // Unverändert: nichts hochladen.
    frame();
    CHECK(r.stats().terrain_uploads == 0);

    // Eine Änderung lädt nur die betroffenen Netze neu hoch.
    static_cast<void>(voxel::apply(
        terrain.get_mut<world::TerrainState>()->data->chunks,
        {.mode = voxel::BrushMode::Remove, .center = {0.0f, 20.0f, 0.0f}, .radius = 5.0f}));
    frame();
    CHECK(r.stats().terrain_uploads > 0);
    CHECK(r.stats().terrain_uploads <= data.meshes.size());

    // Die Mitte des Bilds zeigt beleuchtetes Terrain statt Himmel.
    const auto pixels = device->read_texture(**target, rhi::ResourceState::TransferSrc);
    const usize center = (usize{size / 2} * size + size / 2) * 4;
    const int red = pixels[center];
    const int green = pixels[center + 1];
    const int blue = pixels[center + 2];
    CHECK(blue < green + 40);
    CHECK(red + green + blue > 15);

    // Ohne Terrain werden die Bereiche nach den Frames im Flug wieder frei.
    terrain.destroy();
    for (int i = 0; i < 8; ++i) {
        frame();
    }
    CHECK(r.stats().terrain_chunks == 0);
    CHECK(r.geometry().stats().used == 0);
    CHECK(device->validation_errors() == 0);
}
