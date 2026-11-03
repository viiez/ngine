#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

import ngine.core;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.render;
import ngine.shaders;

// Der Render-Graph auf der GPU: Ein Compute-Pass schreibt ein Muster in eine transiente
// Storage-Textur, ein Grafik-Pass liest sie bindlos und zeichnet sie in ein importiertes Ziel.

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

TEST_CASE("Render graph runs compute and graphics passes on the GPU", "[render][gpu]") {
    using namespace ngine;
    using rhi::ResourceState;
    const auto device = make_device();
    constexpr u32 size = 64;

    struct GradientPush {
        u32 target, width, height;
        f32 blue;
    };
    struct BlitPush {
        u32 texture, sampler;
    };

    auto target =
        device->create_texture({.extent = {size, size}, .format = rhi::Format::RGBA8Unorm});
    auto sampler = device->create_sampler(
        {.filter = rhi::Filter::Nearest, .address = rhi::AddressMode::ClampToEdge});
    auto compute_shader = device->create_shader(shaders::gradient_compute());
    auto vertex = device->create_shader(shaders::fullscreen_vertex());
    auto fragment = device->create_shader(shaders::fullscreen_fragment());
    REQUIRE(target);
    REQUIRE(sampler);
    REQUIRE(compute_shader);
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto gradient = device->create_compute_pipeline({.compute = compute_shader->get()});
    auto blit = device->create_pipeline({.vertex = vertex->get(),
                                         .fragment = fragment->get(),
                                         .color_format = rhi::Format::RGBA8Unorm});
    REQUIRE(gradient);
    REQUIRE(blit);

    render::TexturePool pool{*device};
    bool culled_pass_ran = false;
    // Zwei Frames: Im zweiten kommt die transiente Textur aus dem Vorrat.
    for (int frame = 0; frame < 2; ++frame) {
        render::RenderGraph graph;
        const auto output = graph.import_texture("Ziel", **target, ResourceState::Undefined,
                                                 ResourceState::TransferSrc);
        const auto pattern = graph.create_texture(
            "Muster", {.extent = {size, size}, .format = rhi::Format::RGBA8Unorm});
        const auto unused = graph.create_texture(
            "Ungenutzt", {.extent = {size, size}, .format = rhi::Format::RGBA8Unorm});

        graph.add_pass(
            "Muster",
            [&](render::PassBuilder& pass) { pass.write(pattern, ResourceState::ShaderWrite); },
            [&](render::PassContext& context) {
                const GradientPush push{context.texture(pattern).storage_index(), size, size, 0.5f};
                context.commands.bind_pipeline(**gradient);
                context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                context.commands.dispatch(size / 8, size / 8);
            });
        graph.add_pass(
            "Ungenutzt",
            [&](render::PassBuilder& pass) { pass.write(unused, ResourceState::ColorTarget); },
            [&](render::PassContext&) { culled_pass_ran = true; });
        graph.add_pass(
            "Kopie",
            [&](render::PassBuilder& pass) {
                pass.read(pattern, ResourceState::ShaderRead);
                pass.write(output, ResourceState::ColorTarget);
            },
            [&](render::PassContext& context) {
                const BlitPush push{context.texture(pattern).sampled_index(), (*sampler)->index()};
                context.commands.begin_rendering(context.texture(output), std::nullopt);
                context.commands.bind_pipeline(**blit);
                context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                context.commands.draw(3);
                context.commands.end_rendering();
            });

        device->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
        pool.end_frame();
    }
    CHECK_FALSE(culled_pass_ran);
    CHECK(pool.size() == 1);

    const auto pixels = device->read_texture(**target, ResourceState::TransferSrc);
    // Rot wächst nach rechts, Grün nach unten (Zeile 0 oben), Blau ist 0.5.
    for (const u32 x : {0u, 17u, 63u}) {
        for (const u32 y : {0u, 40u, 63u}) {
            const auto i = (static_cast<std::size_t>(y) * size + x) * 4;
            const auto expected = [](u32 v) {
                return (static_cast<f32>(v) + 0.5f) / 64.0f * 255.0f;
            };
            INFO("x=" << x << " y=" << y);
            CHECK(std::abs(pixels[i] - expected(x)) <= 1.0f);
            CHECK(std::abs(pixels[i + 1] - expected(y)) <= 1.0f);
            CHECK(std::abs(pixels[i + 2] - 127.5f) <= 1.0f);
            CHECK(pixels[i + 3] == 255);
        }
    }
    CHECK(device->validation_errors() == 0);
}
