#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

import ngine.core;
import ngine.rhi;
import ngine.render;

// Planung des Render-Graphs ohne GPU: Importierte Texturen sind Attrappen, die nur Größe und
// Format kennen.

namespace {

using namespace ngine;
using render::PlannedBarrier;
using render::ResourceKind;
using render::ResourceRef;
using rhi::ResourceState;

class FakeTexture final : public rhi::Texture {
public:
    [[nodiscard]] rhi::Extent2D extent() const override {
        return {64, 64};
    }
    [[nodiscard]] rhi::Format format() const override {
        return rhi::Format::BGRA8Srgb;
    }
    [[nodiscard]] rhi::TextureUsage usage() const override {
        return rhi::TextureUsage::RenderTarget;
    }
    [[nodiscard]] u32 mip_levels() const override {
        return 1;
    }
    [[nodiscard]] u32 sampled_index() const override {
        return rhi::invalid_index;
    }
    [[nodiscard]] u32 storage_index() const override {
        return rhi::invalid_index;
    }
};

class FakeBuffer final : public rhi::Buffer {
public:
    [[nodiscard]] u64 size() const override {
        return 256;
    }
    [[nodiscard]] u64 device_address() const override {
        return 0;
    }
    [[nodiscard]] rhi::MemoryUsage memory() const override {
        return rhi::MemoryUsage::Gpu;
    }
    void write(u64, std::span<const std::byte>) override {}
    void read(u64, std::span<std::byte>) const override {}
};

ResourceRef ref(render::TextureHandle texture) {
    return {ResourceKind::Texture, texture.index};
}
ResourceRef ref(render::BufferHandle buffer) {
    return {ResourceKind::Buffer, buffer.index};
}

std::vector<u32> executed(const render::Plan& plan) {
    std::vector<u32> passes;
    for (const auto& pass : plan.passes) {
        passes.push_back(pass.pass);
    }
    return passes;
}

constexpr render::TransientTextureDesc small{.extent = {64, 64},
                                             .format = rhi::Format::RGBA16Float};
constexpr render::TransientTextureDesc depth_desc{.extent = {64, 64},
                                                  .format = rhi::Format::D32Float};

} // namespace

TEST_CASE("Render graph plans barriers between passes", "[render]") {
    FakeTexture backbuffer_texture;
    render::RenderGraph graph;
    const auto backbuffer = graph.import_texture("Backbuffer", backbuffer_texture,
                                                 ResourceState::Undefined, ResourceState::Present);
    const auto pattern = graph.create_texture("Muster", small);
    const auto depth = graph.create_texture("Tiefe", depth_desc);

    graph.add_pass(
        "Compute",
        [&](render::PassBuilder& pass) { pass.write(pattern, ResourceState::ShaderWrite); }, {});
    graph.add_pass("Draw",
                   [&](render::PassBuilder& pass) {
                       pass.read(pattern, ResourceState::ShaderRead);
                       pass.write(backbuffer, ResourceState::ColorTarget);
                       pass.write(depth, ResourceState::DepthTarget);
                   },
                   {});
    REQUIRE(graph.compile());
    const render::Plan& plan = graph.plan();

    REQUIRE(executed(plan) == std::vector<u32>{0, 1});
    CHECK(plan.culled.empty());
    CHECK(plan.passes[0].barriers ==
          std::vector<PlannedBarrier>{
              {ref(pattern), ResourceState::Undefined, ResourceState::ShaderWrite}});
    CHECK(plan.passes[1].barriers ==
          std::vector<PlannedBarrier>{
              {ref(pattern), ResourceState::ShaderWrite, ResourceState::ShaderRead},
              {ref(backbuffer), ResourceState::Undefined, ResourceState::ColorTarget},
              {ref(depth), ResourceState::Undefined, ResourceState::DepthTarget}});
    CHECK(plan.final_barriers ==
          std::vector<PlannedBarrier>{
              {ref(backbuffer), ResourceState::ColorTarget, ResourceState::Present}});

    // Verwendungen der transienten Texturen folgen aus den Zuständen.
    CHECK(plan.usages[pattern.index] == (rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled));
    CHECK(plan.usages[depth.index] == rhi::TextureUsage::RenderTarget);
    CHECK(plan.lifetimes[pattern.index].first == 0);
    CHECK(plan.lifetimes[pattern.index].last == 1);
    CHECK(plan.lifetimes[depth.index].first == 1);
    CHECK(plan.lifetimes[depth.index].last == 1);
}

TEST_CASE("Render graph culls passes whose results nobody uses", "[render]") {
    FakeTexture backbuffer_texture;
    render::RenderGraph graph;
    const auto backbuffer = graph.import_texture("Backbuffer", backbuffer_texture,
                                                 ResourceState::Undefined, ResourceState::Present);
    const auto a = graph.create_texture("A", small);
    const auto b = graph.create_texture("B", small);
    const auto unused = graph.create_texture("Ungenutzt", small);
    const auto debug = graph.create_texture("Debug", small);

    graph.add_pass("Erzeuge A",
                   [&](render::PassBuilder& pass) { pass.write(a, ResourceState::ColorTarget); },
                   {});
    graph.add_pass(
        "Ungenutzt",
        [&](render::PassBuilder& pass) { pass.write(unused, ResourceState::ShaderWrite); }, {});
    graph.add_pass("A nach B",
                   [&](render::PassBuilder& pass) {
                       pass.read(a, ResourceState::ShaderRead);
                       pass.write(b, ResourceState::ShaderWrite);
                   },
                   {});
    graph.add_pass("Debug",
                   [&](render::PassBuilder& pass) {
                       pass.write(debug, ResourceState::ColorTarget);
                       pass.side_effect();
                   },
                   {});
    graph.add_pass("B nach Backbuffer",
                   [&](render::PassBuilder& pass) {
                       pass.read(b, ResourceState::ShaderRead);
                       pass.write(backbuffer, ResourceState::ColorTarget);
                   },
                   {});
    REQUIRE(graph.compile());
    const render::Plan& plan = graph.plan();

    CHECK(executed(plan) == std::vector<u32>{0, 2, 3, 4});
    CHECK(plan.culled == std::vector<u32>{1});
    CHECK(graph.pass_name(plan.culled[0]) == "Ungenutzt");
    // Eine Textur, die nur verworfene Passes benutzen, wird nie angelegt.
    CHECK_FALSE(plan.lifetimes[unused.index].used());
    CHECK(plan.lifetimes[a.index].first == 0);
    CHECK(plan.lifetimes[a.index].last == 2);
    CHECK(plan.lifetimes[b.index].first == 2);
    CHECK(plan.lifetimes[b.index].last == 4);
}

TEST_CASE("Render graph orders writes in the same state with barriers", "[render]") {
    FakeTexture target_texture;
    render::RenderGraph graph;
    const auto target = graph.import_texture("Ziel", target_texture, ResourceState::ColorTarget,
                                             ResourceState::ShaderRead);
    const auto lookup = graph.create_texture("Tabelle", small);

    graph.add_pass(
        "Tabelle",
        [&](render::PassBuilder& pass) { pass.write(lookup, ResourceState::ColorTarget); }, {});
    graph.add_pass("Erster",
                   [&](render::PassBuilder& pass) {
                       pass.read(lookup, ResourceState::ShaderRead);
                       pass.write(target, ResourceState::ColorTarget);
                   },
                   {});
    graph.add_pass("Zweiter",
                   [&](render::PassBuilder& pass) {
                       pass.read(lookup, ResourceState::ShaderRead);
                       pass.write(target, ResourceState::ColorTarget);
                   },
                   {});
    REQUIRE(graph.compile());
    const render::Plan& plan = graph.plan();
    REQUIRE(plan.passes.size() == 3);

    // Auch ohne Zustandswechsel braucht ein Schreiben nach einem Schreiben eine Barriere,
    // zweimal dasselbe Lesen dagegen nicht.
    CHECK(plan.passes[1].barriers ==
          std::vector<PlannedBarrier>{
              {ref(lookup), ResourceState::ColorTarget, ResourceState::ShaderRead},
              {ref(target), ResourceState::ColorTarget, ResourceState::ColorTarget}});
    CHECK(plan.passes[2].barriers ==
          std::vector<PlannedBarrier>{
              {ref(target), ResourceState::ColorTarget, ResourceState::ColorTarget}});
    CHECK(plan.final_barriers ==
          std::vector<PlannedBarrier>{
              {ref(target), ResourceState::ColorTarget, ResourceState::ShaderRead}});
}

TEST_CASE("Render graph tracks buffers", "[render]") {
    FakeBuffer indices_buffer;
    FakeTexture backbuffer_texture;
    render::RenderGraph graph;
    const auto indices = graph.import_buffer("Indizes", indices_buffer, ResourceState::IndexRead,
                                             ResourceState::IndexRead);
    const auto backbuffer = graph.import_texture("Backbuffer", backbuffer_texture,
                                                 ResourceState::Undefined, ResourceState::Present);
    graph.add_pass(
        "Culling",
        [&](render::PassBuilder& pass) { pass.write(indices, ResourceState::ShaderWrite); }, {});
    graph.add_pass("Draw",
                   [&](render::PassBuilder& pass) {
                       pass.read(indices, ResourceState::IndexRead);
                       pass.write(backbuffer, ResourceState::ColorTarget);
                   },
                   {});
    REQUIRE(graph.compile());
    const render::Plan& plan = graph.plan();
    REQUIRE(plan.passes.size() == 2);
    CHECK(plan.passes[0].barriers ==
          std::vector<PlannedBarrier>{
              {ref(indices), ResourceState::IndexRead, ResourceState::ShaderWrite}});
    CHECK(plan.passes[1].barriers[0] ==
          PlannedBarrier{ref(indices), ResourceState::ShaderWrite, ResourceState::IndexRead});
    // Der Puffer endet im gewünschten Zustand, eine Barriere am Ende ist nicht nötig.
    CHECK(plan.final_barriers.size() == 1);
}

TEST_CASE("Render graph without passes still reaches final states", "[render]") {
    FakeTexture backbuffer_texture;
    render::RenderGraph graph;
    const auto backbuffer = graph.import_texture("Backbuffer", backbuffer_texture,
                                                 ResourceState::Undefined, ResourceState::Present);
    REQUIRE(graph.compile());
    CHECK(graph.plan().passes.empty());
    CHECK(graph.plan().final_barriers ==
          std::vector<PlannedBarrier>{
              {ref(backbuffer), ResourceState::Undefined, ResourceState::Present}});

    graph.reset();
    REQUIRE(graph.compile());
    CHECK(graph.plan().final_barriers.empty());
}

TEST_CASE("Render graph reports invalid declarations", "[render]") {
    FakeTexture backbuffer_texture;

    SECTION("write with a reading state") {
        render::RenderGraph graph;
        const auto target = graph.import_texture("Ziel", backbuffer_texture,
                                                 ResourceState::Undefined, ResourceState::Present);
        graph.add_pass(
            "Falsch",
            [&](render::PassBuilder& pass) { pass.write(target, ResourceState::ShaderRead); }, {});
        const auto result = graph.compile();
        REQUIRE_FALSE(result);
        CHECK(result.error().find("Falsch") != std::string::npos);
    }

    SECTION("two states in one pass") {
        render::RenderGraph graph;
        const auto texture = graph.create_texture("T", small);
        graph.add_pass("Doppelt",
                       [&](render::PassBuilder& pass) {
                           pass.write(texture, ResourceState::ShaderWrite);
                           pass.write(texture, ResourceState::ColorTarget);
                           pass.side_effect();
                       },
                       {});
        CHECK_FALSE(graph.compile());
    }

    SECTION("reading a transient texture before any pass writes it") {
        render::RenderGraph graph;
        const auto target = graph.import_texture("Ziel", backbuffer_texture,
                                                 ResourceState::Undefined, ResourceState::Present);
        const auto never_written = graph.create_texture("Leer", small);
        graph.add_pass("Liest",
                       [&](render::PassBuilder& pass) {
                           pass.read(never_written, ResourceState::ShaderRead);
                           pass.write(target, ResourceState::ColorTarget);
                       },
                       {});
        const auto result = graph.compile();
        REQUIRE_FALSE(result);
        CHECK(result.error().find("Leer") != std::string::npos);
    }
}
