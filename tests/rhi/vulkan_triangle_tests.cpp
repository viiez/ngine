#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

import ngine.core;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.shaders;

// Diese Tests brauchen einen Vulkan-Treiber. In der CI unter Linux läuft dafür
// lavapipe (Software-Rendering von Mesa); ohne Treiber werden sie übersprungen.

namespace {

constexpr ngine::u32 size = 64;

std::unique_ptr<ngine::rhi::Device> make_device() {
    auto device = ngine::rhi::vulkan::create_device(
        {.application_name = "NGine Tests", .enable_validation = true});
    if (!device) {
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
    CHECK(caps.api_version.starts_with("1."));
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

    device->execute([&](ngine::rhi::CommandList& commands) {
        commands.begin_rendering(**target, ngine::rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
        commands.bind_pipeline(**pipeline);
        commands.draw(3);
        commands.end_rendering();
    });
    const auto pixels = device->read_texture(**target);
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
