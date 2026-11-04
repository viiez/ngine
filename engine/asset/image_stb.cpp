module;

// Einzige Übersetzungseinheit, die stb_image kennt (ADR 0027).
#include <climits>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO // Dateien liest ngine.filesystem
#define STBI_FAILURE_USERMSG
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

module ngine.asset;

import ngine.core;
import ngine.filesystem;

namespace ngine::asset {

Result<Image> decode_image(std::span<const std::byte> data) {
    if (data.empty() || data.size() > static_cast<usize>(INT_MAX)) {
        return std::unexpected(std::format("Bild: ungültige Größe ({} Bytes)", data.size()));
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()),
                                            static_cast<int>(data.size()), &width, &height,
                                            &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        return std::unexpected(
            std::format("Bild lässt sich nicht dekodieren: {}", stbi_failure_reason()));
    }
    Image image;
    image.width = static_cast<u32>(width);
    image.height = static_cast<u32>(height);
    image.pixels.resize(static_cast<usize>(width) * static_cast<usize>(height) * 4);
    std::memcpy(image.pixels.data(), pixels, image.pixels.size());
    stbi_image_free(pixels);
    return image;
}

Result<Image> load_image(std::string_view path) {
    auto bytes = filesystem::read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto image = decode_image(*bytes);
    if (!image) {
        return std::unexpected(std::format("{}: {}", path, image.error()));
    }
    return image;
}

} // namespace ngine::asset
