module;

#include <ngine/profile.hpp>

#include <chrono>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.cook;

import ngine.core;
import ngine.asset;
import ngine.filesystem;
import ngine.profile;

namespace ngine::cook {

namespace {

// Ruft fn(Bild, srgb, mipmaps) für jede Textur, die ein Material benutzt: Grundfarbe und Emission
// sRGB, alles andere linear (wie der Renderer beim Hochladen).
template <typename Fn>
void for_each_texture(const asset::Model& model, Fn&& fn) {
    const auto visit = [&](const std::optional<u32>& texture, bool srgb) {
        if (!texture || *texture >= model.textures.size()) {
            return;
        }
        const asset::Texture& source = model.textures[*texture];
        if (source.image < model.images.size()) {
            fn(source.image, srgb, source.sampler.mipmaps);
        }
    };
    for (const asset::Material& material : model.materials) {
        visit(material.base_color_texture, true);
        visit(material.metallic_roughness_texture, false);
        visit(material.normal_texture, false);
        visit(material.occlusion_texture, false);
        visit(material.emissive_texture, true);
    }
}

} // namespace

Result<asset::CookedModel> cook_model(std::string_view source, const CookSettings& settings) {
    NGINE_PROFILE_ZONE("cook::cook_model");
    std::vector<std::string> dependencies;
    auto model = asset::load_gltf(source, &dependencies);
    if (!model) {
        return std::unexpected(std::move(model.error()));
    }
    asset::CookedModel cooked;

    // Tangenten nach MikkTSpace, wo die Datei keine hat (TD-21).
    for (asset::Mesh& mesh : model->meshes) {
        for (asset::Primitive& primitive : mesh.primitives) {
            if (primitive.generated_tangents) {
                generate_tangents(primitive);
            }
        }
    }

    // Jedes benutzte Bild einmal je Farbraum als KTX2.
    std::map<std::pair<u32, bool>, bool> wanted;
    for_each_texture(*model, [&](u32 image, bool srgb, bool mipmaps) {
        auto [it, inserted] = wanted.try_emplace({image, srgb}, mipmaps);
        if (!inserted) {
            it->second = it->second || mipmaps;
        }
    });
    for (const auto& [key, mipmaps] : wanted) {
        const asset::Image& image = model->images[key.first];
        auto ktx2 = encode_ktx2(image, key.second, mipmaps, settings.textures);
        if (!ktx2) {
            return std::unexpected(
                std::format("{}: Bild '{}': {}", source, image.name, ktx2.error()));
        }
        // BC7 gleich mit umwandeln, damit Desktop-GPUs beim Laden nichts mehr rechnen.
        asset::TextureData bc7;
        if (auto transcoded = asset::decode_ktx2(*ktx2, asset::TextureFormat::BC7);
            transcoded && transcoded->format == asset::TextureFormat::BC7) {
            bc7 = *std::move(transcoded);
        }
        cooked.textures.push_back({.image = key.first,
                                   .srgb = key.second,
                                   .ktx2 = *std::move(ktx2),
                                   .bc7 = std::move(bc7)});
    }
    for (asset::Image& image : model->images) {
        image.pixels.clear();
        image.pixels.shrink_to_fit();
    }

    // Stempel der Datei und aller Abhängigkeiten, damit der Cache Änderungen erkennt.
    std::vector<std::string> files{filesystem::normalize(source)};
    files.insert(files.end(), dependencies.begin(), dependencies.end());
    for (const std::string& file : files) {
        auto stamp = asset::stamp_file(file);
        if (!stamp) {
            return std::unexpected(std::move(stamp.error()));
        }
        cooked.sources.push_back(*std::move(stamp));
    }
    cooked.model = *std::move(model);
    return cooked;
}

asset::ModelCooker model_cooker(CookSettings settings) {
    return [settings](std::string_view source) { return cook_model(source, settings); };
}

CookReport cook_all(const asset::AssetDatabase& database, const asset::AssetCache& cache,
                    const CookSettings& settings, bool force) {
    NGINE_PROFILE_ZONE("cook::cook_all");
    const auto start = std::chrono::steady_clock::now();
    CookReport report;
    for (const asset::AssetInfo& info : database.assets()) {
        if (info.type != asset::AssetType::Model) {
            continue;
        }
        if (!force && cache.model_current(info.id)) {
            ++report.current;
            continue;
        }
        auto cooked = cook_model(info.file, settings);
        if (cooked) {
            if (auto stored = cache.store_model(info.id, *cooked); !stored) {
                cooked = std::unexpected(std::move(stored.error()));
            }
        }
        if (!cooked) {
            ++report.failed;
            report.errors.push_back(std::format("{}: {}", info.path, cooked.error()));
            log::error("Kochen: {}: {}", info.path, cooked.error());
            continue;
        }
        ++report.cooked;
        log::info("Gekocht: {} ({} Textur(en))", info.path, cooked->textures.size());
    }
    report.seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
    return report;
}

} // namespace ngine::cook
