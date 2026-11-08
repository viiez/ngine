module;

// Einzige Übersetzungseinheit von ngine.cook, die KTX-Software kennt (ADR 0036): Bilder als KTX2
// mit Basis Universal schreiben. Gelesen wird in ngine.asset (ktx_libktx.cpp).
#include <ngine/profile.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <ktx.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

module ngine.cook;

import ngine.core;
import ngine.profile;
import ngine.asset;

namespace ngine::cook {

namespace {

constexpr ktx_uint32_t vk_format_rgba8_unorm = 37;
constexpr ktx_uint32_t vk_format_rgba8_srgb = 43;

struct KtxGuard {
    ktxTexture2* texture = nullptr;
    ~KtxGuard() {
        if (texture != nullptr) {
            ktxTexture_Destroy(ktxTexture(texture));
        }
    }
};

std::string failure(std::string_view what, KTX_error_code code) {
    return std::format("KTX2, {}: {}", what, ktxErrorString(code));
}

} // namespace

Result<std::vector<std::byte>> encode_ktx2(const asset::Image& image, bool srgb, bool mipmaps,
                                           const TextureSettings& settings) {
    NGINE_PROFILE_ZONE("cook::encode_ktx2");
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != usize{image.width} * image.height * 4) {
        return std::unexpected(std::string{"KTX2: Bild ohne gültige RGBA8-Daten"});
    }
    const u32 levels = mipmaps ? asset::mip_count(image.width, image.height) : 1;
    const std::vector<std::byte> chain = asset::mip_chain(image, srgb, levels);

    ktxTextureCreateInfo info{};
    info.vkFormat = srgb ? vk_format_rgba8_srgb : vk_format_rgba8_unorm;
    info.baseWidth = image.width;
    info.baseHeight = image.height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = levels;
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;
    KtxGuard guard;
    KTX_error_code code =
        ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &guard.texture);
    if (code != KTX_SUCCESS) {
        return std::unexpected(failure("anlegen", code));
    }
    ktxTexture2* texture = guard.texture;

    usize offset = 0;
    u32 width = image.width;
    u32 height = image.height;
    for (u32 level = 0; level < levels; ++level) {
        const usize size = usize{width} * height * 4;
        code = ktxTexture_SetImageFromMemory(
            ktxTexture(texture), level, 0, 0,
            reinterpret_cast<const ktx_uint8_t*>(chain.data() + offset), size);
        if (code != KTX_SUCCESS) {
            return std::unexpected(failure(std::format("Stufe {}", level), code));
        }
        offset += size;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }

    if (settings.basis) {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = settings.uastc ? KTX_TRUE : KTX_FALSE;
        params.threadCount = settings.threads != 0
                                 ? settings.threads
                                 : std::max(1u, std::thread::hardware_concurrency());
        if (settings.uastc) {
            params.uastcFlags = std::min(settings.uastc_level, 4u);
        } else {
            params.qualityLevel = std::clamp(settings.etc1s_quality, 1u, 255u);
            params.compressionLevel = 2;
        }
        code = ktxTexture2_CompressBasisEx(texture, &params);
        if (code != KTX_SUCCESS) {
            return std::unexpected(failure("Basis Universal", code));
        }
    }
    // ETC1S bringt seine eigene Superkompression (BasisLZ) mit; zstd nur für UASTC und RGBA8.
    if (settings.zstd_level > 0 && (!settings.basis || settings.uastc)) {
        code = ktxTexture2_DeflateZstd(texture, std::min(settings.zstd_level, 22u));
        if (code != KTX_SUCCESS) {
            return std::unexpected(failure("zstd", code));
        }
    }

    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    code = ktxTexture_WriteToMemory(ktxTexture(texture), &bytes, &size);
    if (code != KTX_SUCCESS) {
        return std::unexpected(failure("schreiben", code));
    }
    std::vector<std::byte> result(reinterpret_cast<const std::byte*>(bytes),
                                  reinterpret_cast<const std::byte*>(bytes) + size);
    // Den Speicher hat libktx mit malloc angelegt.
    std::free(bytes);
    return result;
}

} // namespace ngine::cook
