#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
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

std::unique_ptr<ngine::rhi::Device> make_device(std::optional<ngine::rhi::SurfaceDesc> surface = {},
                                                bool present_fences = true) {
    auto device = ngine::rhi::vulkan::create_device({.application_name = "NGine Tests",
                                                     .enable_validation = true,
                                                     .surface = std::move(surface),
                                                     .present_fences = present_fences});
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
    auto sampler = device->create_sampler({.mag_filter = ngine::rhi::Filter::Nearest,
                                           .min_filter = ngine::rhi::Filter::Nearest,
                                           .address_u = ngine::rhi::AddressMode::ClampToEdge,
                                           .address_v = ngine::rhi::AddressMode::ClampToEdge});
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

TEST_CASE("Vulkan generates mip levels on upload", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    constexpr u32 size = 8;
    CHECK(ngine::rhi::full_mip_count({size, size}) == 4);
    CHECK(ngine::rhi::full_mip_count({5, 1}) == 3);

    // Mip-Stufen nur für Texturen, in die nicht gezeichnet wird.
    CHECK_FALSE(device->create_texture({.extent = {size, size}, .mip_levels = 2}));
    CHECK_FALSE(device->create_texture(
        {.extent = {size, size}, .usage = TextureUsage::Sampled, .mip_levels = 5}));

    // Schachbrett aus einzelnen Pixeln, schwarz und weiß.
    std::vector<u8> pixels(static_cast<std::size_t>(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const auto i = (static_cast<std::size_t>(y) * size + x) * 4;
            const u8 value = (x + y) % 2 == 0 ? 255 : 0;
            pixels[i] = pixels[i + 1] = pixels[i + 2] = value;
            pixels[i + 3] = 255;
        }
    }
    auto source =
        device->create_texture({.extent = {size, size},
                                .format = Format::RGBA8Unorm,
                                .usage = TextureUsage::Sampled | TextureUsage::TransferDst,
                                .mip_levels = 4});
    REQUIRE(source);
    CHECK((*source)->mip_levels() == 4);
    device->upload(**source, std::as_bytes(std::span{pixels}));

    // Ohne Filter in der Stufe, aber linear zwischen den Stufen: Im 2x2-Ziel fallen vier Texel
    // auf einen Pixel, die GPU liest Stufe 2, in der das Schachbrett zu Grau gemittelt ist.
    auto target = device->create_texture({.extent = {2, 2}, .format = Format::RGBA8Unorm});
    auto sampler = device->create_sampler({.mag_filter = ngine::rhi::Filter::Nearest,
                                           .min_filter = ngine::rhi::Filter::Nearest,
                                           .anisotropy = 16.0f});
    auto vertex = device->create_shader(ngine::shaders::fullscreen_vertex());
    auto fragment = device->create_shader(ngine::shaders::fullscreen_fragment());
    REQUIRE(target);
    REQUIRE(sampler);
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto pipeline = device->create_pipeline(
        {.vertex = vertex->get(), .fragment = fragment->get(), .color_format = Format::RGBA8Unorm});
    REQUIRE(pipeline);

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
    for (std::size_t i = 0; i < result.size(); i += 4) {
        CHECK(result[i] > 110);
        CHECK(result[i] < 145);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan pipelines choose the depth comparison", "[rhi][vulkan][gpu]") {
    using ngine::rhi::CompareOp;
    const auto device = make_device();
    auto color = device->create_texture({.extent = {4, 4}, .format = Format::RGBA8Unorm});
    auto depth = device->create_texture(
        {.extent = {4, 4}, .format = Format::D32Float, .usage = TextureUsage::RenderTarget});
    auto source =
        device->create_texture({.extent = {1, 1},
                                .format = Format::RGBA8Unorm,
                                .usage = TextureUsage::Sampled | TextureUsage::TransferDst});
    auto sampler = device->create_sampler({});
    auto vertex = device->create_shader(ngine::shaders::fullscreen_vertex());
    auto fragment = device->create_shader(ngine::shaders::fullscreen_fragment());
    REQUIRE(color);
    REQUIRE(depth);
    REQUIRE(source);
    REQUIRE(sampler);
    REQUIRE(vertex);
    REQUIRE(fragment);
    const std::array<u8, 4> white{255, 255, 255, 255};
    device->upload(**source, std::as_bytes(std::span{white}));

    // Das Vollbild-Dreieck liegt bei Tiefe 0. Ob es gegen die gelöschte Tiefe besteht, hängt
    // vom Vergleich ab.
    const auto drawn = [&](CompareOp compare, float clear_depth) {
        auto pipeline = device->create_pipeline({.vertex = vertex->get(),
                                                 .fragment = fragment->get(),
                                                 .color_format = Format::RGBA8Unorm,
                                                 .depth_format = Format::D32Float,
                                                 .depth_compare = compare});
        REQUIRE(pipeline);
        const BlitPush push{(*source)->sampled_index(), (*sampler)->index()};
        device->execute([&](ngine::rhi::CommandList& commands) {
            const std::array barriers{
                ngine::rhi::TextureBarrier{color->get(), ResourceState::Undefined,
                                           ResourceState::ColorTarget},
                ngine::rhi::TextureBarrier{depth->get(), ResourceState::Undefined,
                                           ResourceState::DepthTarget}};
            commands.barrier(barriers);
            commands.begin_rendering({.color = color->get(),
                                      .clear_color = ngine::rhi::Color{0, 0, 0, 1},
                                      .depth = depth->get(),
                                      .clear_depth = clear_depth});
            commands.bind_pipeline(**pipeline);
            commands.push_constants(std::as_bytes(std::span{&push, 1}));
            commands.draw(3);
            commands.end_rendering();
        });
        return device->read_texture(**color, ResourceState::ColorTarget)[0] == 255;
    };
    CHECK(drawn(CompareOp::LessOrEqual, 1.0f));
    // Umgekehrte Tiefe: gelöscht wird auf 0 (unendlich fern).
    CHECK_FALSE(drawn(CompareOp::Greater, 0.0f));
    CHECK(drawn(CompareOp::GreaterOrEqual, 0.0f));
    CHECK_FALSE(drawn(CompareOp::Greater, 1.0f));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan swapchain rebuilds on resize without stalling", "[rhi][vulkan][gpu]") {
    // Mit Present-Fences (TD-18) und ohne, wie auf GPUs ohne VK_EXT_swapchain_maintenance1.
    const bool present_fences = GENERATE(true, false);
    CAPTURE(present_fences);
    const auto device = make_device(ngine::rhi::vulkan::headless_surface({64, 48}), present_fences);

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

TEST_CASE("Vulkan calibrated timestamps follow the CPU clock", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    const ngine::f32 period = device->capabilities().timestamp_period;
    // Mehrere Versuche: Eine einzelne Messung darf scheitern, wenn der Thread unterbrochen wird.
    const auto sample = [&] {
        for (int i = 0; i < 20; ++i) {
            if (auto value = device->calibrated_timestamp()) {
                return value;
            }
        }
        return device->calibrated_timestamp();
    };
    const auto first = sample();
    if (!first) {
        SKIP("Die GPU kann keine kalibrierten Zeitstempel");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto second = sample();
    REQUIRE(second);
    REQUIRE(second->gpu > first->gpu);
    const double cpu_ms = static_cast<double>(second->cpu_ns - first->cpu_ns) / 1e6;
    const double gpu_ms = static_cast<double>(second->gpu - first->gpu) * period / 1e6;
    CHECK(cpu_ms >= 50.0);
    // Beide Uhren messen dieselbe Zeit, bis auf die Ungenauigkeit der Messung.
    CHECK(std::abs(gpu_ms - cpu_ms) < 5.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan frames return their timestamps when the slot comes around",
          "[rhi][vulkan][gpu]") {
    const auto device = make_device(ngine::rhi::vulkan::headless_surface({32, 32}));
    if (device->capabilities().timestamp_period <= 0.0f) {
        SKIP("Die GPU hat keine Zeitstempel");
    }
    const ngine::u64 before = device->gpu_timestamp();
    CHECK(before > 0);

    const ngine::u32 slots = device->frames_in_flight();
    bool checked = false;
    for (ngine::u32 i = 0; i < slots * 2 + 1; ++i) {
        auto frame = device->begin_frame();
        REQUIRE(frame);
        if (i >= slots) {
            // Der Frame vor slots Frames schrieb drei Zeitstempel, in dieser Reihenfolge.
            REQUIRE(frame->timestamps.size() == 3);
            CHECK(frame->timestamps[0] >= before);
            CHECK(frame->timestamps[0] <= frame->timestamps[1]);
            CHECK(frame->timestamps[1] <= frame->timestamps[2]);
            checked = true;
        } else {
            CHECK(frame->timestamps.empty());
        }
        frame->commands.write_timestamp(0);
        frame->commands.barrier(frame->backbuffer, ResourceState::Undefined,
                                ResourceState::ColorTarget);
        frame->commands.begin_rendering(frame->backbuffer, ngine::rhi::Color{0.1f, 0.2f, 0.3f, 1});
        frame->commands.end_rendering();
        frame->commands.write_timestamp(1);
        frame->commands.barrier(frame->backbuffer, ResourceState::ColorTarget,
                                ResourceState::Present);
        frame->commands.write_timestamp(2);
        // Lücken und Werte über der Grenze werden ignoriert.
        frame->commands.write_timestamp(7);
        frame->commands.write_timestamp(ngine::rhi::max_frame_timestamps);
        device->end_frame();
    }
    CHECK(checked);
    device->wait_idle();
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan draws depth only and samples the depth texture", "[rhi][vulkan][gpu]") {
    const auto device = make_device();
    constexpr ngine::rhi::Extent2D extent{16, 16};
    auto depth =
        device->create_texture({.extent = extent,
                                .format = Format::D32Float,
                                .usage = TextureUsage::RenderTarget | TextureUsage::Sampled});
    auto color = device->create_texture({.extent = extent, .format = Format::RGBA8Unorm});
    auto sampler = device->create_sampler({.mag_filter = ngine::rhi::Filter::Nearest,
                                           .min_filter = ngine::rhi::Filter::Nearest,
                                           .mip_filter = ngine::rhi::Filter::Nearest,
                                           .address_u = ngine::rhi::AddressMode::ClampToEdge,
                                           .address_v = ngine::rhi::AddressMode::ClampToEdge});
    auto triangle = device->create_shader(ngine::shaders::triangle_vertex());
    auto vertex = device->create_shader(ngine::shaders::fullscreen_vertex());
    auto fragment = device->create_shader(ngine::shaders::fullscreen_fragment());
    REQUIRE(depth);
    REQUIRE(color);
    REQUIRE(sampler);
    REQUIRE(triangle);
    REQUIRE(vertex);
    REQUIRE(fragment);

    // Nur Tiefe: kein Farbformat, kein Fragment-Shader, mit Tiefenversatz.
    auto depth_only = device->create_pipeline({.vertex = triangle->get(),
                                               .depth_format = Format::D32Float,
                                               .depth_bias_constant = 1.0f,
                                               .depth_bias_slope = 1.0f});
    REQUIRE(depth_only);
    auto blit = device->create_pipeline(
        {.vertex = vertex->get(), .fragment = fragment->get(), .color_format = Format::RGBA8Unorm});
    REQUIRE(blit);
    // Ohne Farbziel und ohne Tiefe gibt es nichts zu zeichnen.
    CHECK_FALSE(device->create_pipeline({.vertex = triangle->get()}));

    const BlitPush push{(*depth)->sampled_index(), (*sampler)->index()};
    device->execute([&](ngine::rhi::CommandList& commands) {
        const std::array to_depth{ngine::rhi::TextureBarrier{depth->get(), ResourceState::Undefined,
                                                             ResourceState::DepthTarget}};
        commands.barrier(to_depth);
        commands.begin_rendering({.depth = depth->get(), .clear_depth = 1.0f});
        commands.bind_pipeline(**depth_only);
        commands.draw(3);
        commands.end_rendering();

        const std::array to_read{ngine::rhi::TextureBarrier{depth->get(),
                                                            ResourceState::DepthTarget,
                                                            ResourceState::ShaderRead},
                                 ngine::rhi::TextureBarrier{color->get(), ResourceState::Undefined,
                                                            ResourceState::ColorTarget}};
        commands.barrier(to_read);
        commands.begin_rendering(**color, ngine::rhi::Color{0, 0, 0, 1});
        commands.bind_pipeline(**blit);
        commands.push_constants(std::as_bytes(std::span{&push, 1}));
        commands.draw(3);
        commands.end_rendering();
    });
    const auto pixels = device->read_texture(**color, ResourceState::ColorTarget);
    const auto red = [&](u32 x, u32 y) { return pixels[(ngine::usize{y} * extent.width + x) * 4]; };
    // Das Dreieck liegt bei Tiefe 0 in der Mitte; die Ecken behalten die gelöschte Tiefe 1.
    CHECK(red(8, 8) < 8);
    CHECK(red(0, 0) == 255);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan uploads in the background with precomputed mip levels", "[rhi][vulkan][gpu]") {
    // Mit eigener Transfer-Queue (wo es eine gibt) und mit dem Rückfallpfad.
    const bool transfer_queue = GENERATE(true, false);
    auto created = ngine::rhi::vulkan::create_device({.application_name = "NGine Tests",
                                                      .enable_validation = true,
                                                      .transfer_queue = transfer_queue});
    if (!created) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + created.error());
        }
        SKIP("Kein nutzbares Vulkan-Gerät: " + created.error());
    }
    const std::unique_ptr<ngine::rhi::Device> device = *std::move(created);
    if (!transfer_queue) {
        CHECK_FALSE(device->capabilities().transfer_queue);
    }
    INFO("Transfer-Queue: " << device->capabilities().transfer_queue);

    // Jede Stufe einfarbig mit eigenem Rotwert; die GPU darf nichts nachrechnen.
    constexpr u32 size = 8;
    constexpr u32 levels = 4;
    CHECK(ngine::rhi::mip_chain_size({size, size}, levels, 4) == (64 + 16 + 4 + 1) * 4);
    std::vector<u8> chain;
    u32 extent = size;
    for (u32 level = 0; level < levels; ++level) {
        for (u32 i = 0; i < extent * extent; ++i) {
            chain.insert(chain.end(), {static_cast<u8>(10 + level * 40), 0, 0, 255});
        }
        extent /= 2;
    }
    auto texture =
        device->create_texture({.extent = {size, size},
                                .format = Format::RGBA8Unorm,
                                .usage = TextureUsage::Sampled | TextureUsage::TransferDst,
                                .mip_levels = levels});
    REQUIRE(texture);
    const auto ticket = device->upload_async(**texture, std::as_bytes(std::span{chain}));

    // Puffer im GPU-Speicher; die Validation Layers prüfen Kopie und Synchronisation.
    std::vector<u32> numbers(64);
    for (u32 i = 0; i < numbers.size(); ++i) {
        numbers[i] = i * 3 + 1;
    }
    auto buffer = device->create_buffer({.size = numbers.size() * sizeof(u32),
                                         .usage = ngine::rhi::BufferUsage::Storage,
                                         .memory = ngine::rhi::MemoryUsage::Gpu});
    REQUIRE(buffer);
    const auto buffer_ticket = device->upload_async(**buffer, std::as_bytes(std::span{numbers}));
    const auto both = ngine::rhi::later(ticket, buffer_ticket);

    // Eine leere Abgabe gibt die Kopien ab; execute() wartet auch auf die Transfer-Queue.
    device->execute([](ngine::rhi::CommandList&) {});
    CHECK(device->upload_done(ticket));
    CHECK(device->upload_done(both));

    // Im 2x2-Ziel liest die GPU Stufe 2 (ohne Filter zwischen den Stufen nächstliegend).
    auto target = device->create_texture({.extent = {2, 2}, .format = Format::RGBA8Unorm});
    auto sampler = device->create_sampler({.mag_filter = ngine::rhi::Filter::Nearest,
                                           .min_filter = ngine::rhi::Filter::Nearest,
                                           .mip_filter = ngine::rhi::Filter::Nearest});
    auto vertex = device->create_shader(ngine::shaders::fullscreen_vertex());
    auto fragment = device->create_shader(ngine::shaders::fullscreen_fragment());
    REQUIRE(target);
    REQUIRE(sampler);
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto pipeline = device->create_pipeline(
        {.vertex = vertex->get(), .fragment = fragment->get(), .color_format = Format::RGBA8Unorm});
    REQUIRE(pipeline);
    const BlitPush push{(*texture)->sampled_index(), (*sampler)->index()};
    device->execute([&](ngine::rhi::CommandList& commands) {
        commands.barrier(**target, ResourceState::Undefined, ResourceState::ColorTarget);
        commands.begin_rendering(**target, std::nullopt);
        commands.bind_pipeline(**pipeline);
        commands.push_constants(std::as_bytes(std::span{&push, 1}));
        commands.draw(3);
        commands.end_rendering();
    });
    const auto result = device->read_texture(**target, ResourceState::ColorTarget);
    for (std::size_t i = 0; i < result.size(); i += 4) {
        CHECK(static_cast<int>(result[i]) == 10 + 2 * 40);
    }
    CHECK(device->validation_errors() == 0);
}
