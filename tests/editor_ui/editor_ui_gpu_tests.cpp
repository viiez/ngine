#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

import ngine.core;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.render;
import ngine.editor_ui;

// Die Editor-UI auf der GPU (lavapipe in der CI): ImGui-Fenster mit Text und Bild über den Pass
// der UI in eine Textur zeichnen und Pixel prüfen.

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

std::unique_ptr<ngine::rhi::Device> make_device() {
    auto device = ngine::rhi::vulkan::create_device(
        {.application_name = "NGine Tests", .enable_validation = true});
    if (!device) {
        if (gpu_required()) {
            FAIL("Kein nutzbares Vulkan-Gerät: " + device.error());
        }
        SKIP("Kein nutzbares Vulkan-Gerät: " + device.error());
    }
    return std::move(*device);
}

} // namespace

TEST_CASE("Editor UI draws a window with text and an image on the GPU", "[editor_ui][gpu]") {
    using namespace ngine;
    namespace ui = editor_ui;
    using rhi::ResourceState;
    const auto device = make_device();
    constexpr u32 size = 256;
    constexpr rhi::Format format = rhi::Format::RGBA8Unorm;

    auto target = device->create_texture({.extent = {size, size}, .format = format});
    REQUIRE(target);
    // Rotes Bild, das die UI über image() zeigt.
    auto red = device->create_texture(
        {.extent = {4, 4},
         .format = rhi::Format::RGBA8Unorm,
         .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst});
    REQUIRE(red);
    std::vector<u8> red_pixels(4 * 4 * 4);
    for (usize i = 0; i < red_pixels.size(); i += 4) {
        red_pixels[i] = 255;
        red_pixels[i + 3] = 255;
    }
    device->upload(**red, std::as_bytes(std::span{red_pixels}));

    auto renderer = ui::UiRenderer::create(*device, format);
    REQUIRE(renderer);
    render::TexturePool pool{*device};
    ui::Point image_at;
    {
        ui::Context context{**renderer};
        // Zwei Frames: Im ersten passt ImGui die Fenstergröße an den Inhalt an.
        for (int frame = 0; frame < 2; ++frame) {
            context.begin_frame(static_cast<f32>(size), static_cast<f32>(size), 1.0f / 60.0f);
            ui::begin_window("GPU");
            ui::text("Hallo");
            ui::image(ui::texture_id(**red), {32.0f, 32.0f});
            image_at = ui::item_min();
            ui::end_window();
            const ui::UiDrawData& data = context.end_frame();

            render::RenderGraph graph;
            const auto output = graph.import_texture("Ziel", **target, ResourceState::Undefined,
                                                     ResourceState::TransferSrc);
            (*renderer)->add_pass(graph, data, output, {}, rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
            device->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
            pool.end_frame();
        }
        CHECK((*renderer)->draw_count() > 0);
    }

    const auto pixels = device->read_texture(**target, ResourceState::TransferSrc);
    const auto pixel = [&](u32 x, u32 y) {
        const usize i = (usize{y} * size + x) * 4;
        return std::vector<u8>{pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    };
    // Mitte des Bildes: rot (sRGB-Rundreise ändert 0 und 1 nicht).
    const auto center = pixel(static_cast<u32>(image_at.x) + 16, static_cast<u32>(image_at.y) + 16);
    INFO("Bild bei " << image_at.x << ", " << image_at.y);
    CHECK(center[0] >= 250);
    CHECK(center[1] <= 5);
    CHECK(center[2] <= 5);
    // Ecke oben links liegt außerhalb jedes Fensters: Löschfarbe.
    CHECK(pixel(1, 1) == std::vector<u8>{0, 0, 0, 255});
    // Das Fenster selbst hat einen Hintergrund, der nicht schwarz ist (Titelleiste).
    bool window_drawn = false;
    for (u32 x = 0; x < size && !window_drawn; ++x) {
        const auto p = pixel(x, static_cast<u32>(image_at.y) - 4);
        window_drawn = p[0] + p[1] + p[2] > 0;
    }
    CHECK(window_drawn);
    CHECK(device->validation_errors() == 0);
}
