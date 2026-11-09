module;

#include <ngine/profile.hpp>

#include <expected>
#include <utility>

// Texturen aus Bilddateien über den Ressourcen-Manager (R3.2), z. B. für die Materialien des
// Terrains: Mip-Stufen im Hintergrund, Upload in update(), sichtbar erst nach dem Upload.
module ngine.render;

import ngine.core;
import ngine.profile;
import ngine.rhi;
import ngine.asset;
import :renderer_detail;

namespace ngine::render {

GpuTexture::GpuTexture(Renderer& renderer, u32 texture, rhi::UploadTicket ticket)
    : renderer_{&renderer}, texture_{texture}, ticket_{ticket} {}

GpuTexture::~GpuTexture() {
    release();
}

GpuTexture::GpuTexture(GpuTexture&& other) noexcept
    : renderer_{std::exchange(other.renderer_, nullptr)},
      texture_{std::exchange(other.texture_, rhi::invalid_index)}, ticket_{other.ticket_} {}

GpuTexture& GpuTexture::operator=(GpuTexture&& other) noexcept {
    if (this != &other) {
        release();
        renderer_ = std::exchange(other.renderer_, nullptr);
        texture_ = std::exchange(other.texture_, rhi::invalid_index);
        ticket_ = other.ticket_;
    }
    return *this;
}

void GpuTexture::release() noexcept {
    if (renderer_ != nullptr && texture_ != rhi::invalid_index) {
        renderer_->materials().remove_texture(texture_);
    }
    renderer_ = nullptr;
    texture_ = rhi::invalid_index;
}

namespace detail {

namespace {

template <typename T, bool Srgb>
void add_texture_loader(asset::ResourceManager& resources, Renderer& renderer) {
    resources.add_loader<T, asset::TextureData>(
        [](const asset::LoadContext& context) -> asset::Result<asset::TextureData> {
            NGINE_PROFILE_ZONE("render::read_texture");
            auto image = asset::load_image(context.path);
            if (!image) {
                return std::unexpected(std::move(image.error()));
            }
            return asset::texture_from_image(*image, Srgb, true);
        },
        [&renderer](asset::TextureData& data) -> asset::Result<T> {
            rhi::UploadTicket ticket;
            auto index = renderer.materials().add_texture_async(
                {data.width, data.height}, Srgb ? rhi::Format::RGBA8Srgb : rhi::Format::RGBA8Unorm,
                data.levels, data.data, ticket);
            if (!index) {
                return std::unexpected(std::move(index.error()));
            }
            return T{renderer, *index, ticket};
        },
        [&renderer](const T& texture) { return renderer.device().upload_done(texture.ticket()); });
}

} // namespace

void register_texture_loaders(asset::ResourceManager& resources, Renderer& renderer) {
    add_texture_loader<ColorTexture, true>(resources, renderer);
    add_texture_loader<DataTexture, false>(resources, renderer);
}

} // namespace detail

} // namespace ngine::render
