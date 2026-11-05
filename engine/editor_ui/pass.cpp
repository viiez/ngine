module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module ngine.editor_ui;

import ngine.core;
import ngine.profile;
import ngine.rhi;
import ngine.render;
import ngine.shaders;

namespace ngine::editor_ui {

namespace {

// Wie die Push Constants in ui.slang.
struct UiPush {
    u64 vertices = 0;
    f32 scale[2] = {};
    f32 translate[2] = {};
    u32 texture = 0;
    u32 sampler = 0;
    u32 encode_srgb = 0;
};
static_assert(sizeof(UiPush) <= rhi::max_push_constant_size);

bool is_srgb(rhi::Format format) {
    return format == rhi::Format::RGBA8Srgb || format == rhi::Format::BGRA8Srgb;
}

// Puffer eines Frames im Flug; wächst bei Bedarf auf das Doppelte.
struct FrameBuffers {
    std::unique_ptr<rhi::Buffer> vertices;
    std::unique_ptr<rhi::Buffer> indices;
    std::vector<UiCommand> commands;
};

// Beschneidungsrechteck in ganzen Pixeln, innerhalb des Ziels; leer, wenn nichts übrig bleibt.
std::optional<rhi::Rect> scissor(const UiRect& clip, rhi::Extent2D extent) {
    const f32 width = static_cast<f32>(extent.width);
    const f32 height = static_cast<f32>(extent.height);
    const f32 min_x = std::clamp(std::floor(clip.min_x), 0.0f, width);
    const f32 min_y = std::clamp(std::floor(clip.min_y), 0.0f, height);
    const f32 max_x = std::clamp(std::ceil(clip.max_x), 0.0f, width);
    const f32 max_y = std::clamp(std::ceil(clip.max_y), 0.0f, height);
    if (max_x <= min_x || max_y <= min_y) {
        return std::nullopt;
    }
    return rhi::Rect{.x = static_cast<i32>(min_x),
                     .y = static_cast<i32>(min_y),
                     .width = static_cast<u32>(max_x - min_x),
                     .height = static_cast<u32>(max_y - min_y)};
}

} // namespace

struct UiRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::Format target_format = rhi::Format::Unknown;
    std::unique_ptr<rhi::Shader> vertex;
    std::unique_ptr<rhi::Shader> fragment;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Sampler> sampler;
    std::map<TextureId, std::unique_ptr<rhi::Texture>> textures;
    std::vector<FrameBuffers> frames;
    usize next_frame = 0;
    u32 draws = 0;

    // Sorgt dafür, dass buffer mindestens bytes groß ist. Der alte Puffer wird erst freigegeben,
    // wenn die GPU fertig ist (Löschliste des Device).
    rhi::Result<void> reserve(std::unique_ptr<rhi::Buffer>& buffer, u64 bytes,
                              rhi::BufferUsage usage) {
        if (buffer && buffer->size() >= bytes) {
            return {};
        }
        auto created = device->create_buffer({.size = std::max<u64>(bytes * 2, 64 * 1024),
                                              .usage = usage,
                                              .memory = rhi::MemoryUsage::CpuWrite});
        if (!created) {
            return std::unexpected(created.error());
        }
        buffer = *std::move(created);
        return {};
    }
};

rhi::Result<std::unique_ptr<UiRenderer>> UiRenderer::create(rhi::Device& device,
                                                            rhi::Format target_format) {
    auto impl = std::make_unique<Impl>();
    impl->device = &device;
    impl->target_format = target_format;

    auto vertex = device.create_shader(shaders::ui_vertex());
    auto fragment = device.create_shader(shaders::ui_fragment());
    if (!vertex || !fragment) {
        return std::unexpected("Editor-UI, Shader: " +
                               (vertex ? fragment.error() : vertex.error()));
    }
    impl->vertex = *std::move(vertex);
    impl->fragment = *std::move(fragment);

    auto pipeline = device.create_pipeline({.vertex = impl->vertex.get(),
                                            .fragment = impl->fragment.get(),
                                            .color_format = target_format,
                                            .cull_mode = rhi::CullMode::None,
                                            .blend = rhi::BlendMode::Alpha});
    if (!pipeline) {
        return std::unexpected("Editor-UI, Pipeline: " + pipeline.error());
    }
    impl->pipeline = *std::move(pipeline);

    // Linear und am Rand geklemmt, wie die Backends von ImGui.
    auto sampler = device.create_sampler({.mag_filter = rhi::Filter::Linear,
                                          .min_filter = rhi::Filter::Linear,
                                          .mip_filter = rhi::Filter::Linear,
                                          .address_u = rhi::AddressMode::ClampToEdge,
                                          .address_v = rhi::AddressMode::ClampToEdge});
    if (!sampler) {
        return std::unexpected("Editor-UI, Sampler: " + sampler.error());
    }
    impl->sampler = *std::move(sampler);
    impl->frames.resize(std::max(device.frames_in_flight(), 1u));
    return std::unique_ptr<UiRenderer>{new UiRenderer{std::move(impl)}};
}

UiRenderer::UiRenderer(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
UiRenderer::~UiRenderer() = default;

TextureId UiRenderer::create_texture(u32 width, u32 height, std::span<const u8> rgba) {
    if (rgba.size() != usize{width} * height * 4) {
        log::error("Editor-UI: Textur {}x{} mit {} Bytes", width, height, rgba.size());
        return no_texture;
    }
    auto texture = impl_->device->create_texture(
        {.extent = {width, height},
         .format = rhi::Format::RGBA8Unorm,
         .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst});
    if (!texture) {
        log::error("Editor-UI, Textur: {}", texture.error());
        return no_texture;
    }
    // Danach ist sie im Zustand ShaderRead und bleibt es.
    impl_->device->upload(**texture, std::as_bytes(rgba));
    const TextureId id = texture_id(**texture);
    impl_->textures[id] = *std::move(texture);
    return id;
}

void UiRenderer::destroy_texture(TextureId texture) {
    // Das Device gibt sie frei, sobald kein Frame im Flug sie mehr benutzt.
    impl_->textures.erase(texture);
}

void UiRenderer::add_pass(render::RenderGraph& graph, const UiDrawData& draw_data,
                          render::TextureHandle target,
                          std::span<const render::TextureHandle> reads,
                          std::optional<rhi::Color> clear) {
    NGINE_PROFILE_ZONE("UiRenderer::add_pass");
    Impl& r = *impl_;
    verify(graph.texture_desc(target).format == r.target_format,
           "UiRenderer::add_pass: Ziel hat nicht das Format aus create()");
    const usize slot = r.next_frame;
    r.next_frame = (r.next_frame + 1) % r.frames.size();
    FrameBuffers& frame = r.frames[slot];
    frame.commands.clear();
    r.draws = 0;

    u64 vertex_address = 0;
    if (!draw_data.empty()) {
        const std::span vertices{draw_data.vertices};
        const std::span indices{draw_data.indices};
        auto vertex_buffer =
            r.reserve(frame.vertices, vertices.size_bytes(), rhi::BufferUsage::Storage);
        auto index_buffer = r.reserve(frame.indices, indices.size_bytes(),
                                      rhi::BufferUsage::Storage | rhi::BufferUsage::Index);
        if (!vertex_buffer || !index_buffer) {
            log::error("Editor-UI, Puffer: {}",
                       vertex_buffer ? index_buffer.error() : vertex_buffer.error());
        } else {
            frame.vertices->write(0, std::as_bytes(vertices));
            frame.indices->write(0, std::as_bytes(indices));
            vertex_address = frame.vertices->device_address();
            for (const UiCommand& command : draw_data.commands) {
                if (command.texture != no_texture && command.index_count > 0) {
                    frame.commands.push_back(command);
                }
            }
        }
    }
    r.draws = static_cast<u32>(frame.commands.size());

    const f32 width = std::max(draw_data.width, 1.0f);
    const f32 height = std::max(draw_data.height, 1.0f);
    const UiPush base{
        .vertices = vertex_address,
        // Pixel (y nach unten) in Clip-Koordinaten (y nach oben).
        .scale = {2.0f / width, -2.0f / height},
        .translate = {-1.0f, 1.0f},
        .sampler = r.sampler->index(),
        .encode_srgb = is_srgb(r.target_format) ? 0u : 1u,
    };

    graph.add_pass(
        "Editor-UI",
        [&](render::PassBuilder& pass) {
            pass.write(target, rhi::ResourceState::ColorTarget);
            for (const render::TextureHandle read : reads) {
                pass.read(read, rhi::ResourceState::ShaderRead);
            }
        },
        [this, slot, target, clear, base](render::PassContext& context) {
            Impl& r = *impl_;
            const FrameBuffers& frame = r.frames[slot];
            rhi::Texture& texture = context.texture(target);
            context.commands.begin_rendering(texture, clear);
            if (!frame.commands.empty()) {
                context.commands.bind_pipeline(*r.pipeline);
                context.commands.bind_index_buffer(*frame.indices);
                for (const UiCommand& command : frame.commands) {
                    const std::optional<rhi::Rect> rect = scissor(command.clip, texture.extent());
                    if (!rect) {
                        continue;
                    }
                    UiPush push = base;
                    push.texture = static_cast<u32>(command.texture - 1);
                    context.commands.set_scissor(*rect);
                    context.commands.push_constants(std::as_bytes(std::span{&push, 1}));
                    context.commands.draw_indexed(command.index_count, 1, command.first_index);
                }
            }
            context.commands.end_rendering();
        });
}

u32 UiRenderer::draw_count() const noexcept {
    return impl_->draws;
}

} // namespace ngine::editor_ui
