#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

import ngine.core;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.shaders;

// GPU-Tests für Bindless, Uploads, die Löschliste und die Swapchain (R1.2). Wie alle GPU-Tests
// laufen sie in der CI über lavapipe mit Validation Layers samt Synchronisationsprüfung.

namespace {

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

std::unique_ptr<ngine::rhi::Device>
make_device(std::optional<ngine::rhi::SurfaceDesc> surface = {}) {
    auto device = ngine::rhi::vulkan::create_device({.application_name = "NGine Tests",
                                                     .enable_validation = true,
                                                     .surface = std::move(surface)});
    if (!device) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + device.error());
        }
        SKIP("Kein nutzbares Vulkan-Gerät: " + device.error());
    }
    return std::move(*device);
}

} // namespace

namespace {

using ngine::u32;
using ngine::u8;
using ngine::rhi::Format;
using ngine::rhi::ResourceState;
using ngine::rhi::TextureUsage;

struct BlitPush {
    u32 texture;
    u32 sampler;
};

} // namespace

TEST_CASE("Vulkan uploads a texture and samples it bindlessly", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    constexpr u32 size = 32;

    auto source =
        device->create_texture({.extent = {size, size},
                                .format = Format::RGBA8Unorm,
                                .usage = TextureUsage::Sampled | TextureUsage::TransferDst});
    auto target = device->create_texture({.extent = {size, size}, .format = Format::RGBA8Unorm});
    auto sampler = device->create_sampler(
        {.filter = ngine::rhi::Filter::Nearest, .address = ngine::rhi::AddressMode::ClampToEdge});
    auto vertex = device->create_shader(ngine::shaders::fullscreen_vertex());
    auto fragment = device->create_shader(ngine::shaders::fullscreen_fragment());
    REQUIRE(source);
    REQUIRE(target);
    REQUIRE(sampler);
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto pipeline = device->create_pipeline(
        {.vertex = vertex->get(), .fragment = fragment->get(), .color_format = Format::RGBA8Unorm});
    REQUIRE(pipeline);
    CHECK((*source)->sampled_index() != ngine::rhi::invalid_index);
    CHECK((*source)->storage_index() == ngine::rhi::invalid_index);
    CHECK((*target)->sampled_index() == ngine::rhi::invalid_index);

    // Muster: Rot zählt die Spalte, Grün die Zeile (Zeile 0 oben).
    std::vector<u8> pixels(static_cast<std::size_t>(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const auto i = (static_cast<std::size_t>(y) * size + x) * 4;
            pixels[i] = static_cast<u8>(x * 8);
            pixels[i + 1] = static_cast<u8>(y * 8);
            pixels[i + 2] = 200;
            pixels[i + 3] = 255;
        }
    }
    device->upload(**source, std::as_bytes(std::span{pixels}));

    const BlitPush push{(*source)->sampled_index(), (*sampler)->index()};
    device->execute([&](ngine::rhi::CommandList& commands) {
        commands.barrier(**target, ResourceState::Undefined, ResourceState::ColorTarget);
        commands.begin_rendering(**target, std::nullopt);
        commands.bind_pipeline(**pipeline);
        commands.push_constants(std::as_bytes(std::span{&push, 1}));
        commands.draw(3);
        commands.end_rendering();
    });
    const auto result = device->read_texture(**target, ResourceState::ColorTarget);
    // Die Textur erscheint unverändert und richtig herum.
    CHECK(result == pixels);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan draws from buffers in GPU memory filled by upload", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    constexpr u32 size = 32;

    // Gleicher Aufbau wie in voxel.slang (32 Bytes pro Eckpunkt).
    struct Vertex {
        float x, y, z;
        u32 material;
        float nx, ny, nz, reserved;
    };
    struct Push {
        std::array<float, 16> view_projection;
        std::array<float, 4> light;
        std::array<float, 4> camera;
        ngine::u64 vertices;
    };
    const std::array<Vertex, 4> vertices{{{-0.5f, -0.5f, 0.5f, 2, 0, 0, 1, 0},
                                          {0.5f, -0.5f, 0.5f, 2, 0, 0, 1, 0},
                                          {0.5f, 0.5f, 0.5f, 2, 0, 0, 1, 0},
                                          {-0.5f, 0.5f, 0.5f, 2, 0, 0, 1, 0}}};
    const std::array<u32, 6> indices{0, 1, 2, 0, 2, 3};

    auto vertex_buffer =
        device->create_buffer({.size = sizeof(vertices), .memory = ngine::rhi::MemoryUsage::Gpu});
    auto index_buffer = device->create_buffer(
        {.size = sizeof(indices),
         .usage = ngine::rhi::BufferUsage::Storage | ngine::rhi::BufferUsage::Index,
         .memory = ngine::rhi::MemoryUsage::Gpu});
    auto target = device->create_texture({.extent = {size, size}, .format = Format::RGBA8Unorm});
    auto vertex = device->create_shader(ngine::shaders::voxel_vertex());
    auto fragment = device->create_shader(ngine::shaders::voxel_fragment());
    REQUIRE(vertex_buffer);
    REQUIRE(index_buffer);
    REQUIRE(target);
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto pipeline = device->create_pipeline(
        {.vertex = vertex->get(), .fragment = fragment->get(), .color_format = Format::RGBA8Unorm});
    REQUIRE(pipeline);

    device->upload(**vertex_buffer, 0, std::as_bytes(std::span{vertices}));
    device->upload(**index_buffer, 0, std::as_bytes(std::span{indices}));

    const Push push{
        .view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
        .light = {0, 0, 1, 0},
        .camera = {0, 0, 0, 0},
        .vertices = (*vertex_buffer)->device_address(),
    };
    device->execute([&](ngine::rhi::CommandList& commands) {
        commands.barrier(**target, ResourceState::Undefined, ResourceState::ColorTarget);
        commands.begin_rendering(**target, ngine::rhi::Color{0, 0, 0, 1});
        commands.bind_pipeline(**pipeline);
        commands.push_constants(std::as_bytes(std::span{&push, 1}));
        commands.bind_index_buffer(**index_buffer);
        commands.draw_indexed(static_cast<u32>(indices.size()));
        commands.end_rendering();
    });
    const auto pixels = device->read_texture(**target, ResourceState::ColorTarget);
    const auto at = [&](u32 x, u32 y) {
        return pixels[(static_cast<std::size_t>(y) * size + x) * 4 + 2];
    };
    // Mitte: Fels (Blauanteil 0.28), Ecke: Löschfarbe.
    CHECK(at(size / 2, size / 2) > 60);
    CHECK(at(1, 1) == 0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan reuses bindless slots only after the GPU is done", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    const ngine::rhi::TextureDesc desc{.extent = {4, 4},
                                       .format = Format::RGBA8Unorm,
                                       .usage = TextureUsage::Sampled | TextureUsage::TransferDst};
    auto first = device->create_texture(desc);
    REQUIRE(first);
    const u32 slot = (*first)->sampled_index();
    first->reset();

    // Die GPU könnte den Platz noch lesen: Er bleibt gesperrt, bis die nächste Abgabe fertig ist.
    auto second = device->create_texture(desc);
    REQUIRE(second);
    CHECK((*second)->sampled_index() != slot);

    device->execute([](ngine::rhi::CommandList&) {});
    auto third = device->create_texture(desc);
    REQUIRE(third);
    CHECK((*third)->sampled_index() == slot);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan rejects storage use for formats without support", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    // sRGB-Formate taugen auf keiner gängigen GPU als Storage-Textur.
    const auto texture = device->create_texture(
        {.extent = {4, 4}, .format = Format::RGBA8Srgb, .usage = TextureUsage::Storage});
    CHECK_FALSE(texture);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan swapchain rebuilds on resize without stalling", "[rhi][vulkan][gpu]") {
    const auto device = make_device(ngine::rhi::vulkan::headless_surface({64, 48}));

    // Zeichnet einen Frame; liefert die Größe des Backbuffers oder nichts, wenn gerade nicht
    // gezeichnet werden kann.
    const auto draw_frame = [&]() -> std::optional<ngine::rhi::Extent2D> {
        auto frame = device->begin_frame();
        if (!frame) {
            return std::nullopt;
        }
        frame->commands.barrier(frame->backbuffer, ResourceState::Undefined,
                                ResourceState::ColorTarget);
        frame->commands.begin_rendering(frame->backbuffer, ngine::rhi::Color{0.1f, 0.2f, 0.3f, 1});
        frame->commands.end_rendering();
        frame->commands.barrier(frame->backbuffer, ResourceState::ColorTarget,
                                ResourceState::Present);
        const auto extent = frame->backbuffer.extent();
        device->end_frame();
        return extent;
    };

    for (int i = 0; i < 4; ++i) {
        const auto extent = draw_frame();
        REQUIRE(extent);
        CHECK(extent->width == 64);
        CHECK(extent->height == 48);
    }
    // Mehrmals hintereinander, auch während Frames im Flug sind.
    for (const ngine::rhi::Extent2D size : {ngine::rhi::Extent2D{100, 50}, {40, 30}, {128, 72}}) {
        device->resize(size);
        for (int i = 0; i < 3; ++i) {
            const auto extent = draw_frame();
            REQUIRE(extent);
            CHECK(extent->width == size.width);
            CHECK(extent->height == size.height);
        }
    }
    device->wait_idle();
    CHECK(device->validation_errors() == 0);
}
