module;

#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

export module ngine.render:materials;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.asset;

export namespace ngine::render {

// Textur und Sampler eines Materials als Indizes im bindlosen Satz.
struct TextureRef {
    u32 texture = rhi::invalid_index;
    u32 sampler = rhi::invalid_index;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return texture != rhi::invalid_index;
    }
};

enum class AlphaMode : u8 { Opaque, Mask, Blend };

// Material nach glTF Metallic-Roughness, so wie der Renderer es braucht (ADR 0027). Grundfarbe
// und Emission erwarten Texturen im sRGB-Format, die übrigen lineare.
struct Material {
    math::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    math::Vec3 emissive{0.0f, 0.0f, 0.0f};
    f32 normal_scale = 1.0f;
    f32 occlusion_strength = 1.0f;
    TextureRef base_color_texture;
    TextureRef metallic_roughness_texture;
    TextureRef normal_texture;
    TextureRef occlusion_texture;
    TextureRef emissive_texture;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    f32 alpha_cutoff = 0.5f;
    bool double_sided = false;
};

// Material im GPU-Puffer, wie pbr.slang es liest (nur 16-Byte-Felder, damit die Ausrichtung in
// jedem Layout gleich ist).
struct GpuMaterial {
    math::Vec4 base_color;
    math::Vec4 emissive_cutoff;
    math::Vec4 params;
    u32 textures[4];
    u32 samplers[4];
    u32 extra[4];
};
static_assert(sizeof(GpuMaterial) == 96);

[[nodiscard]] GpuMaterial to_gpu(const Material& material);

// Besitzt Materialien, Texturen und Sampler (REND-07). Materialien liegen als Feld in einem Puffer
// im GPU-Speicher; Shader lesen es über die Adresse und den Index aus den Push Constants. Index 0
// ist das Standardmaterial (weiß, nicht metallisch, Roughness 0.5).
class MaterialStore {
public:
    explicit MaterialStore(rhi::Device& device);

    [[nodiscard]] u32 add(const Material& material);
    void set(u32 index, const Material& material);
    // Gibt ein Material frei; add() vergibt den Index später neu. Das Standardmaterial bleibt.
    void remove(u32 index);
    [[nodiscard]] const Material& get(u32 index) const {
        return materials_[index];
    }
    [[nodiscard]] usize size() const noexcept {
        return materials_.size();
    }
    // Belegte Materialien (ohne freigegebene).
    [[nodiscard]] usize count() const noexcept {
        return materials_.size() - free_.size();
    }

    // Lädt ein Bild als Textur mit allen Mip-Stufen hoch; srgb für Grundfarbe und Emission.
    // Liefert den bindlosen Index.
    [[nodiscard]] rhi::Result<u32> add_texture(const asset::Image& image, bool srgb,
                                               bool mipmaps = true);
    // Wie add_texture(), aber mit fertig gerechneten Mip-Stufen in format (RGBA8 aus
    // asset::mip_chain() oder BC7 aus KTX2) und im Hintergrund (Device::upload_async(), TD-17).
    // Benutzt werden darf die Textur erst, wenn das Ticket erledigt ist.
    [[nodiscard]] rhi::Result<u32> add_texture_async(rhi::Extent2D extent, rhi::Format format,
                                                     u32 levels, std::span<const std::byte> chain,
                                                     rhi::UploadTicket& ticket);
    // Gibt eine Textur aus add_texture() frei (über die Löschliste des Device).
    void remove_texture(u32 index);
    [[nodiscard]] usize texture_count() const noexcept {
        return textures_.size();
    }
    // Sampler mit dieser Beschreibung, angelegt beim ersten Gebrauch. Liefert den bindlosen Index.
    [[nodiscard]] rhi::Result<u32> sampler(const rhi::SamplerDesc& desc);

    // Bringt geänderte Materialien in den GPU-Puffer (über Device::upload(), also vor der
    // nächsten Abgabe). Wächst das Feld, entsteht ein neuer Puffer.
    void flush();
    // Adresse des Felds; gültig nach flush().
    [[nodiscard]] u64 address() const;

private:
    rhi::Device* device_;
    std::vector<Material> materials_;
    std::vector<u32> free_;
    std::unique_ptr<rhi::Buffer> buffer_;
    bool dirty_ = true;
    std::vector<std::unique_ptr<rhi::Texture>> textures_;
    std::vector<std::unique_ptr<rhi::Sampler>> samplers_;
    std::map<std::tuple<u8, u8, u8, u8, u8, u32>, u32> sampler_indices_;
};

} // namespace ngine::render
