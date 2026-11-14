module;

#include <ngine/profile.hpp>

#include <expected>
#include <utility>

// Texturen aus Bilddateien über den Ressourcen-Manager (R3.2), z. B. für die Materialien des
// Terrains: Mip-Stufen im Hintergrund, Upload in update(), sichtbar erst nach dem Upload. Seit
// R4.3 gekocht aus dem Cache (KTX2 mit BC7), wie die Texturen der Modelle.
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

// Fertige Pixel aus einer gekochten Textur: BC7, wenn die GPU es kann, sonst aus KTX2 umgewandelt.
asset::Result<asset::TextureData> from_cooked(asset::CookedImage& cooked,
                                              asset::TextureFormat target, bool srgb) {
    asset::CookedTexture& texture = cooked.texture;
    if (target == asset::TextureFormat::BC7 && !texture.bc7.data.empty()) {
        texture.bc7.srgb = srgb;
        return std::move(texture.bc7);
    }
    auto decoded = asset::decode_ktx2(texture.ktx2, target);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    decoded->srgb = srgb;
    return decoded;
}

template <typename T, bool Srgb>
void add_texture_loader(asset::ResourceManager& resources, Renderer& renderer,
                        const asset::AssetCache* cache, asset::ImageCooker cook) {
    const asset::TextureFormat target = renderer.device().capabilities().bc_compression
                                            ? asset::TextureFormat::BC7
                                            : asset::TextureFormat::RGBA8;
    resources.add_loader<T, asset::TextureData>(
        [cache, cook = std::move(cook),
         target](const asset::LoadContext& context) -> asset::Result<asset::TextureData> {
            NGINE_PROFILE_ZONE("render::read_texture");
            // 1. Aktuell gekocht im Cache.
            if (cache != nullptr) {
                auto cached = cache->load_texture(context.id, Srgb);
                if (cached && *cached) {
                    return from_cooked(**cached, target, Srgb);
                }
            }
            // 2. Jetzt kochen und in den Cache legen (Editor).
            if (cook) {
                auto cooked = cook(context.path, Srgb);
                if (cooked) {
                    if (cache != nullptr) {
                        if (auto stored = cache->store_texture(context.id, Srgb, *cooked);
                            !stored) {
                            log::warn("Cache: {}", stored.error());
                        }
                    }
                    return from_cooked(*cooked, target, Srgb);
                }
                log::warn("Textur kochen: {}", cooked.error());
            }
            // 3. Aus der Bilddatei, Mip-Stufen auf der CPU.
            auto image = asset::load_image(context.path);
            if (!image) {
                return std::unexpected(std::move(image.error()));
            }
            return asset::texture_from_image(*image, Srgb, true);
        },
        [&renderer](asset::TextureData& data) -> asset::Result<T> {
            rhi::UploadTicket ticket;
            const rhi::Format format =
                data.format == asset::TextureFormat::BC7
                    ? (Srgb ? rhi::Format::BC7Srgb : rhi::Format::BC7Unorm)
                    : (Srgb ? rhi::Format::RGBA8Srgb : rhi::Format::RGBA8Unorm);
            auto index = renderer.materials().add_texture_async({data.width, data.height}, format,
                                                                data.levels, data.data, ticket);
            if (!index) {
                return std::unexpected(std::move(index.error()));
            }
            return T{renderer, *index, ticket};
        },
        [&renderer](const T& texture) { return renderer.device().upload_done(texture.ticket()); });
}

} // namespace

void register_texture_loaders(asset::ResourceManager& resources, Renderer& renderer,
                              const asset::AssetCache* cache, asset::ImageCooker cook) {
    add_texture_loader<ColorTexture, true>(resources, renderer, cache, cook);
    add_texture_loader<DataTexture, false>(resources, renderer, cache, std::move(cook));
}

} // namespace detail

} // namespace ngine::render
