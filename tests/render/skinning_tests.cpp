#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <optional>
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
import ngine.animation;

// Skinning auf der GPU (R4.6): Eine Säule aus zwei Knochen, deren oberer Teil der Clip um 90° zur
// Seite knickt. Mesh-Shader-Pfad und Rückfallpfad müssen dasselbe Bild zeigen.

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

// Säule von y = 0 bis 2, 0,4 m breit, zur Kamera (+z) gewandt. Unten Knochen A, oben Knochen B,
// in der Mitte halb und halb. Der Clip „Knicken“ dreht B um 90° um z: Die obere Hälfte zeigt dann
// nach links (-x).
asset::Model bending_column() {
    asset::Model model;
    model.nodes.resize(3);
    model.nodes[0] = {.name = "Wurzel", .children = {1}};
    model.nodes[1] = {.name = "Knochen_A", .parent = 0u, .children = {2}};
    model.nodes[2] = {.name = "Knochen_B", .translation = {0.0f, 1.0f, 0.0f}, .parent = 1u};
    model.roots = {0};
    asset::Skin skin;
    skin.name = "Skelett";
    skin.joints = {1, 2};
    skin.inverse_bind = {
        math::mat4_identity(),
        math::compose({0.0f, -1.0f, 0.0f}, math::quat_identity(), {1.0f, 1.0f, 1.0f})};
    model.skins.push_back(skin);
    model.materials.push_back({.name = "Weiß",
                               .base_color = {0.9f, 0.9f, 0.9f, 1.0f},
                               .metallic = 0.0f,
                               .roughness = 0.8f,
                               .emissive = {0.6f, 0.6f, 0.6f}});

    asset::Primitive primitive;
    primitive.material = 0;
    constexpr u32 rows = 9;
    for (u32 row = 0; row < rows; ++row) {
        const f32 y = 2.0f * static_cast<f32>(row) / static_cast<f32>(rows - 1);
        for (const f32 x : {-0.2f, 0.2f}) {
            asset::Vertex vertex{};
            vertex.position = {x, y, 0.0f};
            vertex.normal = {0.0f, 0.0f, 1.0f};
            vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            vertex.uv = {x + 0.5f, y * 0.5f};
            primitive.vertices.push_back(vertex);
            asset::SkinVertex weights{};
            if (y < 1.0f) {
                weights.joints[0] = 0;
                weights.weights[0] = 65535;
            } else if (y == 1.0f) {
                weights.joints[0] = 0;
                weights.joints[1] = 1;
                weights.weights[0] = 32768;
                weights.weights[1] = 32767;
            } else {
                weights.joints[0] = 1;
                weights.weights[0] = 65535;
            }
            primitive.skin.push_back(weights);
        }
    }
    for (u32 row = 0; row + 1 < rows; ++row) {
        const u32 a = row * 2;
        primitive.indices.insert(primitive.indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
    model.meshes.push_back({.name = "Säule", .primitives = {primitive}, .skin = 0u});
    model.nodes[0].mesh = 0u;
    model.nodes[0].skin = 0u;

    asset::Animation bend;
    bend.name = "Knicken";
    bend.duration = 1.0f;
    const math::Quat quarter = math::angle_axis(std::numbers::pi_v<f32> / 2.0f, {0.0f, 0.0f, 1.0f});
    bend.channels.push_back({.node = 2,
                             .path = asset::AnimationPath::Rotation,
                             .interpolation = asset::Interpolation::Linear,
                             .times = {0.0f, 1.0f},
                             .values = {quarter.x, quarter.y, quarter.z, quarter.w, quarter.x,
                                        quarter.y, quarter.z, quarter.w}});
    model.animations.push_back(bend);
    return model;
}

struct Picture {
    std::vector<u8> pixels;
    render::RenderStats stats;
    bool mesh_shaders = false;
};

constexpr u32 size = 64;

// Zeichnet die Säule mit Animator (animate) oder ohne. Ohne Vulkan-Gerät nullopt.
std::optional<Picture> draw_column(bool baseline, bool animate) {
    auto created = rhi::vulkan::create_device({.application_name = "NGine Tests",
                                               .enable_validation = true,
                                               .disabled_features = {.mesh_shader = baseline}});
    if (!created) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + created.error());
        }
        return std::nullopt;
    }
    const std::unique_ptr<rhi::Device> device = *std::move(created);
    auto target = device->create_texture(
        {.extent = {size, size},
         .format = rhi::Format::RGBA8Unorm,
         .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc});
    REQUIRE(target);
    auto renderer = render::Renderer::create(*device, rhi::Format::RGBA8Unorm);
    REQUIRE(renderer);
    render::Renderer& r = **renderer;
    r.lighting = {.sky_color = {0.0f, 0.0f, 0.0f}, .ground_color = {0.0f, 0.0f, 0.0f}};

    const asset::Model model = bending_column();
    const asset::AssetId model_id = asset::generated_id("test.saeule");
    const asset::AssetId graph_id = asset::generated_id("test.saeule.automat");
    asset::ResourceManager resources;
    auto uploaded = render::upload_model(r, model);
    REQUIRE(uploaded);
    static_cast<void>(resources.add(model_id, *std::move(uploaded)));
    auto rig = animation::make_rig(model);
    REQUIRE(rig);
    static_cast<void>(resources.add<animation::Rig>(model_id, *std::move(rig)));
    auto graph = animation::parse_anim_graph(
        R"({ "format": "ngine.animgraph", "version": 1, "model": )" + std::to_string(model_id) +
        R"(, "initial": "Knick", "states": [ { "name": "Knick", "clip": "Knicken" } ] })");
    REQUIRE(graph);
    static_cast<void>(resources.add<animation::AnimGraph>(graph_id, *std::move(graph)));
    r.set_resources(&resources);
    animation::AnimationSystem animation{resources};
    r.set_animation(&animation);

    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    animation::register_types(types);
    world::World world{types};
    world::Entity column = world.create("Säule");
    column.set(world::Transform{})
        .set(render::MeshRenderer{.mesh = {.asset = model_id, .index = 0}});
    if (animate) {
        column.set(animation::Animator{.graph = graph_id});
    }
    world.create("Kamera")
        .set(world::Transform{.position = {0.0, 1.0, 4.0}})
        .set(render::Camera{.fov_y = math::radians(45.0f)});
    world::update_transforms(world);

    render::TexturePool pool{*device};
    for (u32 frame = 0; frame < device->frames_in_flight() + 3; ++frame) {
        animation.update(world, 1.0 / 60.0);
        render::RenderGraph graph_passes;
        const auto output = graph_passes.import_texture(
            "Ziel", **target, rhi::ResourceState::Undefined, rhi::ResourceState::TransferSrc);
        r.add_passes(graph_passes, world, output);
        device->execute([&](rhi::CommandList& commands) { graph_passes.execute(commands, pool); });
        pool.end_frame();
    }
    CHECK(device->validation_errors() == 0);
    Picture picture{.pixels = device->read_texture(**target, rhi::ResourceState::TransferSrc),
                    .stats = r.stats(),
                    .mesh_shaders = r.stats().mesh_shaders};
    r.set_animation(nullptr);
    return picture;
}

// Helligkeit (Rot) an einem Punkt der Ebene z = 0, gesehen von der Kamera bei (0, 1, 4).
int brightness(const Picture& picture, f32 x, f32 y) {
    const f32 half = 4.0f * std::tan(math::radians(22.5f));
    const auto px = static_cast<u32>(std::clamp(32.0f + x / half * 32.0f, 0.0f, 63.0f));
    const auto py = static_cast<u32>(std::clamp(32.0f - (y - 1.0f) / half * 32.0f, 0.0f, 63.0f));
    return picture.pixels[(static_cast<std::size_t>(py) * size + px) * 4];
}

} // namespace

TEST_CASE("GPU skinning bends a mesh the same on both drawing paths", "[render][gpu]") {
    const auto rest = draw_column(false, false);
    if (!rest) {
        SKIP("Kein nutzbares Vulkan-Gerät");
    }
    const auto bent = draw_column(false, true);
    const auto bent_baseline = draw_column(true, true);
    REQUIRE(bent);
    REQUIRE(bent_baseline);

    // Ohne Animator bleibt die Säule gerade (Bindepose) und wird nicht verformt.
    CHECK(rest->stats.skinned == 0);
    CHECK(brightness(*rest, 0.0f, 1.7f) > 100);
    CHECK(brightness(*rest, -0.7f, 1.0f) < 20);

    // Mit Animator knickt die obere Hälfte nach links.
    for (const Picture* picture : {&*bent, &*bent_baseline}) {
        INFO("Mesh Shader " << picture->mesh_shaders);
        CHECK(picture->stats.skinned == 1);
        CHECK(picture->stats.skinned_vertices == 18);
        CHECK(brightness(*picture, 0.0f, 0.5f) > 100);
        CHECK(brightness(*picture, -0.7f, 1.0f) > 100);
        CHECK(brightness(*picture, 0.0f, 1.7f) < 20);
    }
    CHECK_FALSE(bent_baseline->mesh_shaders);

    // Beide Pfade zeigen dasselbe Bild (kleine Abweichungen an Kanten erlaubt).
    usize differing = 0;
    for (usize i = 0; i < bent->pixels.size(); ++i) {
        if (std::abs(static_cast<int>(bent->pixels[i]) -
                     static_cast<int>(bent_baseline->pixels[i])) > 8) {
            ++differing;
        }
    }
    CHECK(differing <= bent->pixels.size() / 200);
}
