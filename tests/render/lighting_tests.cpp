#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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

// Licht, Schatten und Debug-Zeichnen (R1.4): Rechnungen ohne GPU, dann Bilder über lavapipe.

using namespace ngine;
using Catch::Approx;
using math::DVec3;
using math::Vec3;
using math::Vec4;

namespace {

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    return types;
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

// Schattenkoordinaten eines kamerarelativen Punkts in Texeln der Karte.
math::Vec2 shadow_texel(const render::Cascade& cascade, Vec3 p, u32 resolution) {
    const Vec4 s = cascade.view_projection * math::extend(p, 1.0f);
    const f32 half = static_cast<f32>(resolution) * 0.5f;
    return {s.x * half, s.y * half};
}

} // namespace

TEST_CASE("Cascade splits grow toward the shadow distance", "[render]") {
    const auto splits = render::cascade_splits(0.1f, 100.0f, 0.75f);
    for (u32 i = 1; i < render::cascade_count; ++i) {
        CHECK(splits[i] > splits[i - 1]);
    }
    CHECK(splits[0] > 0.1f);
    CHECK(splits.back() == 100.0f);
    // Gleichmäßig: gleiche Abstände.
    const auto uniform = render::cascade_splits(0.0f, 80.0f, 0.0f);
    CHECK(uniform[0] == Approx(20.0f).margin(1e-3f));
    CHECK(uniform[1] == Approx(40.0f).margin(1e-3f));
    // Logarithmisch: die erste Kaskade ist viel kürzer.
    const auto logarithmic = render::cascade_splits(0.1f, 100.0f, 1.0f);
    CHECK(logarithmic[0] < 1.0f);
}

TEST_CASE("Cascades cover their slice of the view frustum", "[render]") {
    const render::ShadowSettings settings{.distance = 60.0f, .resolution = 1024};
    const render::CascadeView view{.position = {3.0, 2.0, 1.0},
                                   .rotation = math::to_mat3(math::angle_axis(0.7f, {0, 1, 0})),
                                   .fov_y = math::radians(60.0f),
                                   .aspect = 16.0f / 9.0f,
                                   .near_plane = 0.1f};
    const Vec3 to_sun = math::normalize(Vec3{0.4f, 0.8f, 0.3f});
    const auto cascades = render::fit_cascades(view, to_sun, settings);

    const Vec3 right = view.rotation[0];
    const Vec3 up = view.rotation[1];
    const Vec3 forward = -view.rotation[2];
    const f32 tan_y = std::tan(view.fov_y * 0.5f);
    const f32 tan_x = tan_y * view.aspect;
    f32 start = view.near_plane;
    for (const render::Cascade& cascade : cascades) {
        INFO("Kaskade bis " << cascade.split_far);
        CHECK(cascade.blend_start < cascade.split_far);
        CHECK(cascade.texel_size == Approx(2.0f * cascade.radius / 1024.0f));
        for (const f32 depth : {start, cascade.split_far}) {
            for (const f32 sx : {-1.0f, 1.0f}) {
                for (const f32 sy : {-1.0f, 1.0f}) {
                    const Vec3 corner =
                        forward * depth + right * (sx * tan_x * depth) + up * (sy * tan_y * depth);
                    const Vec4 s = cascade.view_projection * math::extend(corner, 1.0f);
                    CHECK(std::abs(s.x) <= 1.0001f);
                    CHECK(std::abs(s.y) <= 1.0001f);
                    CHECK(s.z >= 0.0f);
                    CHECK(s.z <= 1.0f);
                }
            }
        }
        // Werfer zwischen Kaskade und Sonne zählen, Werfer weit hinter ihr nicht.
        CHECK(cascade.may_cast(cascade.center + to_sun * (cascade.radius + 50.0f), 1.0f));
        CHECK_FALSE(cascade.may_cast(cascade.center - to_sun * (cascade.radius + 5.0f), 1.0f));
        CHECK_FALSE(
            cascade.may_cast(cascade.center + cascade.axis_x * (cascade.radius + 5.0f), 1.0f));
        start = cascade.blend_start;
    }
    // Sonne: Tiefe 0 an der Sonne, 1 von ihr weg.
    const Vec4 toward = cascades[0].view_projection *
                        math::extend(cascades[0].center + to_sun * cascades[0].radius, 1.0f);
    const Vec4 away = cascades[0].view_projection *
                      math::extend(cascades[0].center - to_sun * cascades[0].radius, 1.0f);
    CHECK(toward.z < away.z);
}

TEST_CASE("Cascades snap to whole texels as the camera moves", "[render]") {
    constexpr u32 resolution = 1024;
    const render::ShadowSettings settings{.distance = 50.0f, .resolution = resolution};
    const Vec3 to_sun = math::normalize(Vec3{0.3f, 0.8f, 0.5f});
    // 10 000 km vom Ursprung: Eingerastet wird in 64 Bit.
    const DVec3 start{1.0e7, 30.0, -2.0e6};
    const render::CascadeView a{.position = start,
                                .rotation = math::mat3_identity(),
                                .fov_y = math::radians(60.0f),
                                .aspect = 1.5f,
                                .near_plane = 0.1f};
    render::CascadeView b = a;
    b.position = start + DVec3{0.0137, 0.0021, -0.0213};
    b.rotation = math::to_mat3(math::angle_axis(0.05f, {0, 1, 0}));
    const auto first = render::fit_cascades(a, to_sun, settings);
    const auto second = render::fit_cascades(b, to_sun, settings);

    // Ein fester Punkt der Welt liegt in beiden Frames auf demselben Texelgitter.
    const DVec3 point = start + DVec3{1.0, 0.0, -6.0};
    for (u32 i = 0; i < render::cascade_count; ++i) {
        INFO("Kaskade " << i);
        CHECK(first[i].radius == second[i].radius);
        const auto t1 = shadow_texel(first[i], math::to_f32(point - a.position), resolution);
        const auto t2 = shadow_texel(second[i], math::to_f32(point - b.position), resolution);
        const f32 dx = t1.x - t2.x;
        const f32 dy = t1.y - t2.y;
        CHECK(std::abs(dx - std::round(dx)) < 0.02f);
        CHECK(std::abs(dy - std::round(dy)) < 0.02f);
    }
}

TEST_CASE("Lights are collected relative to the camera", "[render]") {
    const auto types = make_types();
    world::World world{types};
    const DVec3 camera{1.0e6, 0.0, 0.0};
    // Leuchtet nach unten: die Sonne steht oben.
    world.create("Sonne")
        .set(world::Transform{.rotation = math::look_rotation({0, -1, 0}, {0, 0, -1})})
        .set(render::DirectionalLight{.color = {1.0f, 0.5f, 0.25f}, .intensity = 2.0f});
    world.create("Mond")
        .set(world::Transform{.rotation = math::look_rotation({1, 0, 0}, {0, 1, 0})})
        .set(render::DirectionalLight{.intensity = 0.1f});
    world.create("Aus").set(world::Transform{}).set(render::PointLight{.active = false});
    world.create("Lampe")
        .set(world::Transform{.position = camera + DVec3{1.0, 2.0, 3.0}})
        .set(render::PointLight{.color = {0.0f, 1.0f, 0.0f}, .intensity = 5.0f, .range = 8.0f});
    world.create("Spot")
        .set(world::Transform{.position = camera + DVec3{0.0, 4.0, 0.0},
                              .rotation = math::look_rotation({0, -1, 0}, {0, 0, -1})})
        .set(render::SpotLight{.inner_angle = 0.2f, .outer_angle = 0.4f});
    world::update_transforms(world);

    const render::LightList list = render::collect_lights(world, camera);
    REQUIRE(list.sun);
    CHECK(math::near(list.sun->to_sun, {0.0f, 1.0f, 0.0f}, 1e-5f));
    CHECK(math::near(list.sun->radiance, {2.0f, 1.0f, 0.5f}, 1e-5f));
    CHECK(list.sun->cast_shadows);
    REQUIRE(list.lights.size() == 3);

    const auto find = [&](render::LightType type) -> const render::GpuLight* {
        for (const render::GpuLight& light : list.lights) {
            if (static_cast<u32>(light.color_type.w) == static_cast<u32>(type)) {
                return &light;
            }
        }
        return nullptr;
    };
    REQUIRE(find(render::LightType::Directional) != nullptr);
    REQUIRE(find(render::LightType::Point) != nullptr);
    REQUIRE(find(render::LightType::Spot) != nullptr);
    const render::GpuLight& moon = *find(render::LightType::Directional);
    CHECK(math::near(math::xyz(moon.direction), {1.0f, 0.0f, 0.0f}, 1e-5f));
    const render::GpuLight& lamp = *find(render::LightType::Point);
    CHECK(math::near(lamp.position_range, {1.0f, 2.0f, 3.0f, 8.0f}, 1e-5f));
    CHECK(math::near(math::xyz(lamp.color_type), {0.0f, 5.0f, 0.0f}, 1e-5f));
    const render::GpuLight& spot = *find(render::LightType::Spot);
    CHECK(math::near(math::xyz(spot.direction), {0.0f, -1.0f, 0.0f}, 1e-5f));
    // Im inneren Kegel volle Stärke, am äußeren Rand null.
    const auto cone = [&](f32 angle) {
        return std::clamp(std::cos(angle) * spot.spot.x + spot.spot.y, 0.0f, 1.0f);
    };
    CHECK(cone(0.1f) == Approx(1.0f));
    CHECK(cone(0.4f) == Approx(0.0f).margin(1e-4f));
    CHECK(cone(0.3f) > 0.0f);
    CHECK(cone(0.3f) < 1.0f);

    CHECK(types.find<render::PointLight>() != nullptr);
    CHECK_FALSE(types.get<render::SpotLight>().transient);
}

TEST_CASE("DebugDraw builds lines for shapes", "[render]") {
    render::DebugDraw debug;
    debug.line({0, 0, 0}, {1, 0, 0}, {1, 1, 1});
    CHECK(debug.lines().size() == 1);
    debug.aabb({-1, -1, -1}, {1, 2, 3}, {1, 0, 0});
    CHECK(debug.lines().size() == 13);
    // Jede Kante des Quaders ist parallel zu einer Achse.
    for (usize i = 1; i < debug.lines().size(); ++i) {
        const DVec3 d = debug.lines()[i].b - debug.lines()[i].a;
        const int axes = (d.x != 0.0) + (d.y != 0.0) + (d.z != 0.0);
        CHECK(axes == 1);
    }
    debug.sphere({0, 0, 0}, 2.0f, {0, 1, 0}, 16);
    CHECK(debug.lines().size() == 13 + 3 * 16);
    debug.axes(math::Affine::from({5, 0, 0}, math::quat_identity(), {2, 2, 2}), 0.5f);
    REQUIRE(debug.lines().size() == 13 + 48 + 3);
    CHECK(math::near(debug.lines().back().b, DVec3{5.0, 0.0, 0.5}, 1e-6));

    debug.clear();
    debug.enabled = false;
    debug.sphere({0, 0, 0}, 1.0f, {1, 1, 1});
    CHECK(debug.lines().empty());
}

TEST_CASE("Debug lines are clipped at the near plane", "[render]") {
    const Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 a{0.0f, 0.0f, 1.0f};
    Vec3 b{0.0f, 0.0f, -3.0f};
    REQUIRE(render::clip_line(a, b, forward, 1.0f));
    CHECK(math::near(a, {0.0f, 0.0f, -1.0f}, 1e-6f));
    CHECK(math::near(b, {0.0f, 0.0f, -3.0f}, 1e-6f));
    Vec3 c{0.0f, 0.0f, 2.0f};
    Vec3 d{1.0f, 0.0f, 0.5f};
    CHECK_FALSE(render::clip_line(c, d, forward, 0.1f));

    const DVec3 camera{1.0e7, 0.0, 0.0};
    const std::vector<render::DebugLine> lines{
        {camera + DVec3{-1, 0, -5}, camera + DVec3{1, 0, -5}, {1, 0, 0}},
        {camera + DVec3{0, 0, 2}, camera + DVec3{0, 1, 3}, {0, 1, 0}},
    };
    const auto prepared = render::prepare_lines(lines, camera, forward, 0.1f);
    REQUIRE(prepared.size() == 1);
    CHECK(math::near(prepared[0].a, {-1.0f, 0.0f, -5.0f, 1.0f}, 1e-5f));
}

TEST_CASE("make_box has outward faces", "[render]") {
    const asset::Primitive box = asset::make_box({1.0f, 2.0f, 3.0f});
    REQUIRE(box.vertices.size() == 24);
    REQUIRE(box.indices.size() == 36);
    for (usize i = 0; i < box.indices.size(); i += 3) {
        const Vec3 p0 = box.vertices[box.indices[i]].position;
        const Vec3 p1 = box.vertices[box.indices[i + 1]].position;
        const Vec3 p2 = box.vertices[box.indices[i + 2]].position;
        const Vec3 n = math::cross(p1 - p0, p2 - p0);
        // Gegen den Uhrzeigersinn von außen: die Flächennormale zeigt vom Mittelpunkt weg.
        CHECK(math::dot(n, p0) > 0.0f);
        CHECK(math::dot(n, box.vertices[box.indices[i]].normal) > 0.0f);
    }
}

// --- GPU ------------------------------------------------------------------------------------

namespace {

struct Scene {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Texture> target;
    std::unique_ptr<render::Renderer> renderer;
    // Nach dem Renderer, damit die Modelle vor ihm freigegeben werden.
    std::unique_ptr<asset::ResourceManager> resources;
    u32 models = 0;
    static constexpr u32 size = 96;

    static std::optional<Scene> create() {
        auto created = rhi::vulkan::create_device(
            {.application_name = "NGine Tests", .enable_validation = true});
        if (!created) {
            if (gpu_required()) {
                FAIL("Kein nutzbares Vulkan-Gerät: " + created.error());
            }
            return std::nullopt;
        }
        Scene scene;
        scene.device = *std::move(created);
        auto target = scene.device->create_texture(
            {.extent = {size, size},
             .format = rhi::Format::RGBA8Unorm,
             .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc});
        REQUIRE(target);
        scene.target = *std::move(target);
        auto renderer = render::Renderer::create(*scene.device, rhi::Format::RGBA8Unorm);
        REQUIRE(renderer);
        scene.renderer = *std::move(renderer);
        scene.resources = std::make_unique<asset::ResourceManager>();
        scene.renderer->set_resources(scene.resources.get());
        // Kleine Schattenkarten, damit lavapipe schnell bleibt; kaum Umgebungslicht.
        scene.renderer->shadows.resolution = 512;
        scene.renderer->lighting = {.sky_color = {0.01f, 0.01f, 0.01f},
                                    .ground_color = {0.01f, 0.01f, 0.01f}};
        return scene;
    }

    // Lädt ein Mesh hoch und meldet es als erzeugtes Modell an (R2.4); liefert den Verweis für
    // MeshRenderer.
    asset::AssetRef mesh(const asset::Primitive& primitive, const render::Material& material) {
        const u32 index = renderer->materials().add(material);
        const std::vector<render::SubmeshData> submeshes{
            {.vertices = primitive.vertices, .indices = primitive.indices, .material = index}};
        auto added = renderer->meshes().add(submeshes);
        REQUIRE(added);
        render::ModelInstance instance;
        instance.meshes.push_back(*added);
        instance.materials.push_back(index);
        const asset::AssetId id = asset::generated_id("test.mesh." + std::to_string(++models));
        resources->add(id, render::GpuModel{*renderer, std::move(instance), {}});
        return {.asset = id, .index = 0};
    }

    std::vector<u8> render(world::World& world) {
        world::update_transforms(world);
        render::TexturePool pool{*device};
        render::RenderGraph graph;
        const auto output = graph.import_texture("Ziel", *target, rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::TransferSrc);
        renderer->add_passes(graph, world, output);
        device->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
        return device->read_texture(*target, rhi::ResourceState::TransferSrc);
    }
};

// Rotwert des Pixels, auf das ein Weltpunkt fällt.
int red_at(const std::vector<u8>& pixels, world::World& world, DVec3 point) {
    const auto camera = render::find_camera(world);
    REQUIRE(camera);
    const Vec4 clip = render::view_projection(*camera, 1.0f) *
                      math::extend(math::to_f32(point - camera->position), 1.0f);
    const f32 size = static_cast<f32>(Scene::size);
    const auto x = static_cast<usize>((clip.x / clip.w * 0.5f + 0.5f) * size);
    const auto y = static_cast<usize>((0.5f - clip.y / clip.w * 0.5f) * size);
    REQUIRE(x < Scene::size);
    REQUIRE(y < Scene::size);
    return pixels[(y * Scene::size + x) * 4];
}

world::Entity look_at(world::World& world, DVec3 eye, DVec3 target) {
    const Vec3 forward = math::to_f32(target - eye);
    return world.create("Kamera")
        .set(world::Transform{.position = eye,
                              .rotation = math::look_rotation(forward, {0.0f, 1.0f, 0.0f})})
        .set(render::Camera{.fov_y = math::radians(60.0f)});
}

} // namespace

TEST_CASE("Renderer casts sun shadows onto the ground", "[render][gpu]") {
    auto scene = Scene::create();
    if (!scene) {
        SKIP("Kein nutzbares Vulkan-Gerät");
    }
    const asset::AssetRef ground =
        scene->mesh(asset::make_box({10.0f, 0.1f, 10.0f}),
                    {.base_color = {0.8f, 0.8f, 0.8f, 1.0f}, .roughness = 0.9f});
    const asset::AssetRef cube =
        scene->mesh(asset::make_box({0.5f, 0.5f, 0.5f}),
                    {.base_color = {0.8f, 0.8f, 0.8f, 1.0f}, .roughness = 0.9f});

    const auto types = make_types();
    world::World world{types};
    const DVec3 origin{-4.0e6, 10.0, 7.0e6};
    world.create("Boden")
        .set(world::Transform{.position = origin + DVec3{0.0, -0.1, 0.0}})
        .set(render::MeshRenderer{.mesh = ground});
    world.create("Würfel")
        .set(world::Transform{.position = origin + DVec3{0.0, 1.5, 0.0}})
        .set(render::MeshRenderer{.mesh = cube});
    // Sonne genau von oben: Der Schatten liegt unter dem Würfel.
    world.create("Sonne")
        .set(world::Transform{.rotation = math::look_rotation({0, -1, 0}, {0, 0, -1})})
        .set(render::DirectionalLight{.intensity = 3.0f});
    look_at(world, origin + DVec3{0.0, 3.0, 6.0}, origin);

    const auto pixels = scene->render(world);
    const int shadow = red_at(pixels, world, origin + DVec3{0.0, 0.0, 0.2});
    const int lit = red_at(pixels, world, origin + DVec3{2.0, 0.0, 0.5});
    INFO("Schatten " << shadow << ", Licht " << lit);
    CHECK(lit > 150);
    CHECK(shadow < 60);
    CHECK(scene->renderer->stats().shadow_draws >= 2);
    CHECK(scene->renderer->stats().lights == 1);

    // Ohne Schatten ist dieselbe Stelle hell.
    scene->renderer->shadows.enabled = false;
    const auto unshadowed = scene->render(world);
    CHECK(red_at(unshadowed, world, origin + DVec3{0.0, 0.0, 0.2}) > 150);
    CHECK(scene->renderer->stats().shadow_draws == 0);
    CHECK(scene->device->validation_errors() == 0);
}

TEST_CASE("Renderer lights the ground with a point light", "[render][gpu]") {
    auto scene = Scene::create();
    if (!scene) {
        SKIP("Kein nutzbares Vulkan-Gerät");
    }
    const asset::AssetRef ground =
        scene->mesh(asset::make_box({10.0f, 0.1f, 10.0f}),
                    {.base_color = {0.8f, 0.8f, 0.8f, 1.0f}, .roughness = 0.9f});
    const auto types = make_types();
    world::World world{types};
    world.create("Boden")
        .set(world::Transform{.position = {0.0, -0.1, 0.0}})
        .set(render::MeshRenderer{.mesh = ground});
    world.create("Lampe")
        .set(world::Transform{.position = {0.0, 1.0, 0.0}})
        .set(render::PointLight{.color = {1.0f, 0.2f, 0.2f}, .intensity = 4.0f, .range = 4.0f});
    world.create("Kamera")
        .set(world::Transform{.position = {0.0, 5.0, 0.0},
                              .rotation = math::look_rotation({0, -1, 0}, {0, 0, -1})})
        .set(render::Camera{.fov_y = math::radians(60.0f)});

    const auto pixels = scene->render(world);
    const int center = red_at(pixels, world, {0.0, 0.0, 0.0});
    const int edge = red_at(pixels, world, {2.8, 0.0, 2.8});
    INFO("Mitte " << center << ", Rand " << edge);
    CHECK(center > 150);
    // Außerhalb der Reichweite dunkel.
    CHECK(edge < 10);
    CHECK(scene->renderer->stats().lights == 1);
    CHECK(scene->device->validation_errors() == 0);
}

TEST_CASE("Renderer draws debug lines with depth test", "[render][gpu]") {
    auto scene = Scene::create();
    if (!scene) {
        SKIP("Kein nutzbares Vulkan-Gerät");
    }
    const auto types = make_types();
    world::World world{types};
    const DVec3 origin{2.0e6, 0.0, -1.0e6};
    world.create("Kamera")
        .set(world::Transform{.position = origin})
        .set(render::Camera{.fov_y = math::radians(60.0f)});
    render::DebugDraw& debug = scene->renderer->debug();
    debug.width = 3.0f;
    debug.line(origin + DVec3{-5.0, 0.0, -5.0}, origin + DVec3{5.0, 0.0, -5.0}, {1.0f, 0.0f, 0.0f});
    // Ganz hinter der Kamera: wird verworfen.
    debug.line(origin + DVec3{0.0, 0.0, 1.0}, origin + DVec3{0.0, 1.0, 2.0}, {0.0f, 1.0f, 0.0f});

    const auto pixels = scene->render(world);
    CHECK(scene->renderer->stats().debug_lines == 1);
    CHECK(debug.lines().empty());
    const int on_line = red_at(pixels, world, origin + DVec3{1.0, 0.0, -5.0});
    const int off_line = red_at(pixels, world, origin + DVec3{1.0, 1.0, -5.0});
    INFO("Linie " << on_line << ", daneben " << off_line);
    CHECK(on_line > 150);
    CHECK(off_line < 20);

    // Der nächste Frame hat keine Linien mehr.
    const auto empty = scene->render(world);
    CHECK(scene->renderer->stats().debug_lines == 0);
    CHECK(red_at(empty, world, origin + DVec3{1.0, 0.0, -5.0}) < 20);
    CHECK(scene->device->validation_errors() == 0);
}
