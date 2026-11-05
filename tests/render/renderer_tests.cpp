#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.asset;
import ngine.world;
import ngine.render;

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

TEST_CASE("MeshRenderer is not saved with scenes", "[render]") {
    const auto types = make_types();
    CHECK(types.get<render::MeshRenderer>().transient);
    CHECK_FALSE(types.get<render::Camera>().transient);
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

    const auto types = make_types();
    world::World world{types};
    // Weit vom Ursprung: kamerarelatives Zeichnen hält die Kugel trotzdem genau in der Mitte.
    const math::DVec3 origin{5.0e6, 2.0e3, -3.0e6};
    world.create("Kugel")
        .set(world::Transform{.position = origin})
        .set(render::MeshRenderer{.mesh = *mesh});
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
