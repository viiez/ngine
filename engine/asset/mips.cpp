module;

#include <ngine/profile.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

module ngine.asset;

import ngine.core;
import ngine.profile;

namespace ngine::asset {

namespace {

f32 srgb_to_linear(f32 value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

f32 linear_to_srgb(f32 value) {
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// Lineare Werte aller 256 sRGB-Stufen.
const std::array<f32, 256>& srgb_table() {
    static const std::array<f32, 256> table = [] {
        std::array<f32, 256> values{};
        for (usize i = 0; i < values.size(); ++i) {
            values[i] = srgb_to_linear(static_cast<f32>(i) / 255.0f);
        }
        return values;
    }();
    return table;
}

std::byte to_byte(f32 value) {
    const f32 clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<std::byte>(static_cast<u32>(std::lround(clamped * 255.0f)));
}

} // namespace

u32 mip_count(u32 width, u32 height) noexcept {
    u32 size = width > height ? width : height;
    u32 levels = 1;
    while (size > 1) {
        size /= 2;
        ++levels;
    }
    return levels;
}

std::vector<std::byte> mip_chain(const Image& image, bool srgb, u32 levels) {
    NGINE_PROFILE_ZONE("asset::mip_chain");
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != usize{image.width} * image.height * 4) {
        return {};
    }
    const u32 full = mip_count(image.width, image.height);
    if (levels == 0 || levels > full) {
        levels = full;
    }
    const auto& table = srgb_table();

    std::vector<std::byte> chain(image.pixels.begin(), image.pixels.end());
    usize source = 0;
    u32 width = image.width;
    u32 height = image.height;
    for (u32 level = 1; level < levels; ++level) {
        const u32 next_width = width > 1 ? width / 2 : 1;
        const u32 next_height = height > 1 ? height / 2 : 1;
        const usize target = chain.size();
        chain.resize(target + usize{next_width} * next_height * 4);
        for (u32 y = 0; y < next_height; ++y) {
            // Ungerade Kanten: die letzte Zeile bzw. Spalte fällt weg, wie beim Blit der GPU mit
            // abgerundeter Größe.
            const u32 y0 = height > 1 ? y * 2 : 0;
            const u32 y1 = height > 1 ? y0 + 1 : 0;
            for (u32 x = 0; x < next_width; ++x) {
                const u32 x0 = width > 1 ? x * 2 : 0;
                const u32 x1 = width > 1 ? x0 + 1 : 0;
                const std::array<usize, 4> texels{
                    source + (usize{y0} * width + x0) * 4, source + (usize{y0} * width + x1) * 4,
                    source + (usize{y1} * width + x0) * 4, source + (usize{y1} * width + x1) * 4};
                const usize out = target + (usize{y} * next_width + x) * 4;
                for (u32 channel = 0; channel < 4; ++channel) {
                    f32 sum = 0.0f;
                    for (const usize texel : texels) {
                        const auto value = std::to_integer<u32>(chain[texel + channel]);
                        sum +=
                            srgb && channel < 3 ? table[value] : static_cast<f32>(value) / 255.0f;
                    }
                    const f32 mean = sum * 0.25f;
                    chain[out + channel] =
                        to_byte(srgb && channel < 3 ? linear_to_srgb(mean) : mean);
                }
            }
        }
        source = target;
        width = next_width;
        height = next_height;
    }
    return chain;
}

} // namespace ngine::asset
