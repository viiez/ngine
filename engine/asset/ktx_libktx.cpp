module;

// Einzige Übersetzungseinheit von ngine.asset, die KTX-Software kennt (ADR 0036): KTX2 lesen und
// Basis Universal umwandeln. Geschrieben wird KTX2 in ngine.cook.
#include <ngine/profile.hpp>

#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <ktx.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

module ngine.asset;

import ngine.core;
import ngine.profile;

namespace ngine::asset {

namespace {

// Gibt die Textur beim Verlassen frei.
struct KtxGuard {
    ktxTexture2* texture = nullptr;
    ~KtxGuard() {
        if (texture != nullptr) {
            ktxTexture_Destroy(ktxTexture(texture));
        }
    }
};

std::string error_text(KTX_error_code code) {
    return ktxErrorString(code);
}

} // namespace

Result<TextureData> decode_ktx2(std::span<const std::byte> data, TextureFormat target) {
    NGINE_PROFILE_ZONE("asset::decode_ktx2");
    KtxGuard guard;
    KTX_error_code code =
        ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(data.data()), data.size(),
                                     KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &guard.texture);
    if (code != KTX_SUCCESS) {
        return std::unexpected(std::format("KTX2 lässt sich nicht lesen: {}", error_text(code)));
    }
    ktxTexture2* texture = guard.texture;
    if (texture->numDimensions != 2 || texture->numFaces != 1 || texture->numLayers != 1 ||
        texture->isArray) {
        return std::unexpected(std::string{"KTX2: nur einfache 2D-Texturen werden unterstützt"});
    }
    const bool srgb = ktxTexture2_GetOETF(texture) == KHR_DF_TRANSFER_SRGB;

    TextureFormat format = TextureFormat::RGBA8;
    if (ktxTexture2_NeedsTranscoding(texture)) {
        const ktx_transcode_fmt_e wanted =
            target == TextureFormat::BC7 ? KTX_TTF_BC7_RGBA : KTX_TTF_RGBA32;
        code = ktxTexture2_TranscodeBasis(texture, wanted, 0);
        if (code != KTX_SUCCESS) {
            return std::unexpected(std::format(
                "KTX2: Basis Universal lässt sich nicht umwandeln: {}", error_text(code)));
        }
        format = target;
    } else if (texture->vkFormat != 37 /* VK_FORMAT_R8G8B8A8_UNORM */ &&
               texture->vkFormat != 43 /* VK_FORMAT_R8G8B8A8_SRGB */) {
        return std::unexpected(
            std::format("KTX2: Format {} wird nicht unterstützt (nur Basis Universal und RGBA8)",
                        texture->vkFormat));
    }

    TextureData result{.format = format,
                       .srgb = srgb,
                       .width = texture->baseWidth,
                       .height = texture->baseHeight,
                       .levels = texture->numLevels,
                       .data = {}};
    const u64 expected = texture_size(format, result.width, result.height, result.levels);
    result.data.resize(expected);
    const ktx_uint8_t* source = ktxTexture_GetData(ktxTexture(texture));
    const ktx_size_t source_size = ktxTexture_GetDataSize(ktxTexture(texture));
    u64 offset = 0;
    for (u32 level = 0; level < result.levels; ++level) {
        ktx_size_t level_offset = 0;
        code = ktxTexture_GetImageOffset(ktxTexture(texture), level, 0, 0, &level_offset);
        const ktx_size_t size = ktxTexture_GetImageSize(ktxTexture(texture), level);
        if (code != KTX_SUCCESS || level_offset + size > source_size ||
            offset + size > result.data.size()) {
            return std::unexpected(std::format("KTX2: Stufe {} passt nicht", level));
        }
        std::memcpy(result.data.data() + offset, source + level_offset, size);
        offset += size;
    }
    if (offset != expected) {
        return std::unexpected(std::string{"KTX2: Größe der Stufen passt nicht zum Format"});
    }
    return result;
}

} // namespace ngine::asset
