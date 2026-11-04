#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

import ngine.core;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.shaders;

// Diese Tests brauchen einen Vulkan-Treiber. In der CI unter Linux läuft dafür
// lavapipe (Software-Rendering von Mesa); ohne Treiber werden sie übersprungen.
// Ist NGINE_REQUIRE_GPU gesetzt (CI unter Linux), schlagen sie stattdessen fehl, damit
// ein fehlendes Gerät nicht als grüner Lauf durchgeht.

namespace {

constexpr ngine::u32 size = 64;

bool gpu_required() {
// getenv gilt bei MSVC als unsicher (C4996); hier wird nur gelesen.
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

std::unique_ptr<ngine::rhi::Device> make_device(ngine::rhi::Features disabled = {}) {
    auto device = ngine::rhi::vulkan::create_device({.application_name = "NGine Tests",
                                                     .enable_validation = true,
                                                     .disabled_features = disabled});
    if (!device) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + device.error());
        }
        SKIP("Kein nutzbares Vulkan-Gerät: " + device.error());
    }
    return std::move(*device);
}

struct Pixel {
    ngine::u8 r, g, b, a;
};

Pixel pixel_at(const std::vector<ngine::u8>& pixels, ngine::u32 x, ngine::u32 y) {
    const auto i = (static_cast<ngine::usize>(y) * size + x) * 4;
    return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
}

} // namespace

TEST_CASE("Vulkan-Device meldet Capabilities", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    const auto& caps = device->capabilities();

    CHECK_FALSE(caps.device_name.empty());
    // Mindestanforderung ist Vulkan 1.3 (ADR 0017).
    REQUIRE(caps.api_version.starts_with("1."));
    CHECK(std::stoi(caps.api_version.substr(2)) >= 3);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan-Device laesst abgeschaltete Stufen aus", "[rhi][vulkan][gpu]") {
    const auto device = make_device({.mesh_shader = true, .ray_tracing = true});
    const auto& features = device->capabilities().features;

    CHECK_FALSE(features.mesh_shader);
    CHECK_FALSE(features.ray_tracing);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan zeichnet ein Dreieck offscreen", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    INFO("Gerät: " << device->capabilities().device_name);

    auto target =
        device->create_texture({.extent = {size, size}, .format = ngine::rhi::Format::RGBA8Unorm});
    REQUIRE(target);
    auto vertex = device->create_shader(ngine::shaders::triangle_vertex());
    REQUIRE(vertex);
    auto fragment = device->create_shader(ngine::shaders::triangle_fragment());
    REQUIRE(fragment);
    auto pipeline = device->create_pipeline({
        .vertex = vertex->get(),
        .fragment = fragment->get(),
        .color_format = ngine::rhi::Format::RGBA8Unorm,
    });
    REQUIRE(pipeline);

    using ngine::rhi::ResourceState;
    device->execute([&](ngine::rhi::CommandList& commands) {
        commands.barrier(**target, ResourceState::Undefined, ResourceState::ColorTarget);
        commands.begin_rendering(**target, ngine::rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
        commands.bind_pipeline(**pipeline);
        commands.draw(3);
        commands.end_rendering();
    });
    const auto pixels = device->read_texture(**target, ResourceState::ColorTarget);
    REQUIRE(pixels.size() == static_cast<ngine::usize>(size) * size * 4);

    // Die Mitte liegt im Dreieck und ist farbig.
    const Pixel center = pixel_at(pixels, size / 2, size / 2);
    CHECK(center.a == 255);
    CHECK(center.r + center.g + center.b > 100);

    // Die obere linke Ecke liegt außerhalb und behält die Löschfarbe.
    const Pixel corner = pixel_at(pixels, 1, 1);
    CHECK(corner.r == 0);
    CHECK(corner.g == 0);
    CHECK(corner.b == 0);
    CHECK(corner.a == 255);

    // Die Spitze zeigt nach oben: knapp unter der Spitze ist Farbe, knapp darüber nicht.
    // Zeile 0 ist oben; die Spitze liegt bei y = 0.6, also Zeile 64 * 0.2 = 12.8.
    CHECK(pixel_at(pixels, size / 2, 16).a == 255);
    CHECK(pixel_at(pixels, size / 2, 16).r + pixel_at(pixels, size / 2, 16).g > 0);
    CHECK(pixel_at(pixels, size / 2, 8).r == 0);

    CHECK(device->validation_errors() == 0);
}

namespace {

// Gleicher Aufbau wie in voxel.slang (32 Bytes pro Eckpunkt, 104 Bytes Push Constants).
struct TestVertex {
    float x, y, z;
    ngine::u32 material;
    float nx, ny, nz, reserved;
};

struct TestPushConstants {
    std::array<float, 16> view_projection;
    std::array<float, 4> light;
    std::array<float, 4> camera;
    ngine::u64 vertices;
};

} // namespace

TEST_CASE("Vulkan zeichnet indiziert mit Tiefe, Puffern und Push Constants", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    using ngine::rhi::Format;

    auto target = device->create_texture({.extent = {size, size}, .format = Format::RGBA8Unorm});
    auto depth = device->create_texture({.extent = {size, size}, .format = Format::D32Float});
    REQUIRE(target);
    REQUIRE(depth);
    auto vertex = device->create_shader(ngine::shaders::voxel_vertex());
    auto fragment = device->create_shader(ngine::shaders::voxel_fragment());
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto pipeline = device->create_pipeline({
        .vertex = vertex->get(),
        .fragment = fragment->get(),
        .color_format = Format::RGBA8Unorm,
        .depth_format = Format::D32Float,
        .cull_mode = ngine::rhi::CullMode::Back,
    });
    REQUIRE(pipeline);

    // Ein nahes Viereck in der Mitte (Material 2, Fels), ein fernes über alles (Material 3, Erde)
    // und ein ganz nahes oben links mit falschem Umlaufsinn, das wegfallen muss.
    auto quad = [](float x0, float y0, float x1, float y1, float z, ngine::u32 material) {
        return std::array{TestVertex{x0, y0, z, material, 0, 0, 1, 0},
                          TestVertex{x1, y0, z, material, 0, 0, 1, 0},
                          TestVertex{x1, y1, z, material, 0, 0, 1, 0},
                          TestVertex{x0, y1, z, material, 0, 0, 1, 0}};
    };
    std::array<TestVertex, 12> vertices{};
    const auto near_quad = quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.25f, 2);
    const auto far_quad = quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.75f, 3);
    const auto culled_quad = quad(-1.0f, 0.5f, -0.5f, 1.0f, 0.0f, 2);
    std::ranges::copy(near_quad, vertices.begin());
    std::ranges::copy(far_quad, vertices.begin() + 4);
    std::ranges::copy(culled_quad, vertices.begin() + 8);
    // Nahes Viereck zuerst: Ohne Tiefentest würde das ferne es überdecken.
    const std::array<ngine::u32, 18> indices{0, 1, 2, 0, 2,  3, 4, 5,  6,
                                             4, 6, 7, 8, 10, 9, 8, 11, 10};

    auto vertex_buffer = device->create_buffer({.size = sizeof(vertices)});
    auto index_buffer = device->create_buffer(
        {.size = sizeof(indices),
         .usage = ngine::rhi::BufferUsage::Storage | ngine::rhi::BufferUsage::Index});
    REQUIRE(vertex_buffer);
    REQUIRE(index_buffer);
    CHECK((*vertex_buffer)->device_address() != 0);
    (*vertex_buffer)->write(0, std::as_bytes(std::span{vertices}));
    (*index_buffer)->write(0, std::as_bytes(std::span{indices}));

    const TestPushConstants push{
        .view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
        .light = {0, 0, 1, 0},
        .camera = {0, 0, 0, 0},
        .vertices = (*vertex_buffer)->device_address(),
    };
    static_assert(sizeof(push) <= ngine::rhi::max_push_constant_size);

    using ngine::rhi::ResourceState;
    device->execute([&](ngine::rhi::CommandList& commands) {
        const std::array barriers{
            ngine::rhi::TextureBarrier{target->get(), ResourceState::Undefined,
                                       ResourceState::ColorTarget},
            ngine::rhi::TextureBarrier{depth->get(), ResourceState::Undefined,
                                       ResourceState::DepthTarget},
        };
        commands.barrier(barriers);
        commands.begin_rendering({.color = target->get(),
                                  .clear_color = ngine::rhi::Color{0, 0, 0, 1},
                                  .depth = depth->get(),
                                  .clear_depth = 1.0f});
        commands.bind_pipeline(**pipeline);
        commands.push_constants(std::as_bytes(std::span{&push, 1}));
        commands.bind_index_buffer(**index_buffer);
        commands.draw_indexed(static_cast<ngine::u32>(indices.size()));
        commands.end_rendering();
    });
    const auto pixels = device->read_texture(**target, ResourceState::ColorTarget);

    // Mitte: Fels (Blauanteil 0.28), Rand: Erde (Blauanteil 0.12).
    const Pixel center = pixel_at(pixels, size / 2, size / 2);
    const Pixel edge = pixel_at(pixels, size / 2, size - 2);
    INFO("Mitte " << int(center.r) << "," << int(center.g) << "," << int(center.b) << " Rand "
                  << int(edge.r) << "," << int(edge.g) << "," << int(edge.b));
    CHECK(center.b > 60);
    CHECK(edge.b < 40);
    CHECK(edge.r > 80);
    // Oben links liegt das verworfene Viereck; dort ist das ferne zu sehen (Zeile 0 ist oben).
    const Pixel corner = pixel_at(pixels, 2, 2);
    CHECK(corner.b < 40);
    CHECK(corner.r > 80);

    CHECK(device->validation_errors() == 0);
}
