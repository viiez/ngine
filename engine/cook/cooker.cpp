module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

module ngine.cook;

import ngine.core;
import ngine.asset;
import ngine.filesystem;
import ngine.jobs;
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

namespace {

// Ein Modell für cook_all() und BackgroundCook.
struct CookItem {
    asset::AssetId id = 0;
    std::string path;
    std::string file;
};

std::vector<CookItem> models_of(const asset::AssetDatabase& database) {
    std::vector<CookItem> items;
    for (const asset::AssetInfo& info : database.assets()) {
        if (info.type == asset::AssetType::Model) {
            items.push_back({info.id, info.path, info.file});
        }
    }
    return items;
}

enum class CookOutcome : u8 { cooked, current, failed };

// Kocht ein Modell in den Cache; bei einem Fehler steht er in error.
CookOutcome cook_one(const CookItem& item, const asset::AssetCache& cache,
                     const CookSettings& settings, bool force, std::string& error) {
    if (!force && cache.model_current(item.id)) {
        return CookOutcome::current;
    }
    auto cooked = cook_model(item.file, settings);
    if (cooked) {
        if (auto stored = cache.store_model(item.id, *cooked); !stored) {
            cooked = std::unexpected(std::move(stored.error()));
        }
    }
    if (!cooked) {
        error = std::format("{}: {}", item.path, cooked.error());
        log::error("Kochen: {}", error);
        return CookOutcome::failed;
    }
    log::info("Gekocht: {} ({} Textur(en))", item.path, cooked->textures.size());
    return CookOutcome::cooked;
}

f64 seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

CookReport cook_all(const asset::AssetDatabase& database, const asset::AssetCache& cache,
                    const CookSettings& settings, bool force) {
    NGINE_PROFILE_ZONE("cook::cook_all");
    const auto start = std::chrono::steady_clock::now();
    CookReport report;
    for (const CookItem& item : models_of(database)) {
        std::string error;
        switch (cook_one(item, cache, settings, force, error)) {
        case CookOutcome::cooked: ++report.cooked; break;
        case CookOutcome::current: ++report.current; break;
        case CookOutcome::failed:
            ++report.failed;
            report.errors.push_back(std::move(error));
            break;
        }
    }
    report.seconds = seconds_since(start);
    return report;
}

// --- Kochen im Hintergrund (TD-34) ------------------------------------------------------------

struct BackgroundCook::State {
    std::vector<CookItem> items;
    const asset::AssetCache* cache = nullptr;
    CookSettings settings;
    bool force = false;
    std::chrono::steady_clock::time_point start;
    std::atomic<usize> finished{0};
    std::atomic<usize> failed{0};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    jobs::Counter counter;
    mutable std::mutex mutex;
    CookReport report;
};

BackgroundCook::BackgroundCook(jobs::JobSystem& jobs, const asset::AssetDatabase& database,
                               const asset::AssetCache& cache, CookSettings settings, bool force)
    : state_{std::make_shared<State>()}, jobs_{&jobs} {
    State& state = *state_;
    state.items = models_of(database);
    state.cache = &cache;
    state.force = force;
    state.start = std::chrono::steady_clock::now();
    // Die Kodierung der Bilder nutzt sonst alle Kerne.
    if (settings.textures.threads == 0) {
        settings.textures.threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    }
    state.settings = std::move(settings);
    jobs.run(
        [shared = state_] {
            NGINE_PROFILE_ZONE("cook::BackgroundCook");
            State& s = *shared;
            for (const CookItem& item : s.items) {
                if (s.cancelled.load(std::memory_order_relaxed)) {
                    break;
                }
                std::string error;
                const CookOutcome outcome = cook_one(item, *s.cache, s.settings, s.force, error);
                {
                    const std::scoped_lock lock{s.mutex};
                    switch (outcome) {
                    case CookOutcome::cooked: ++s.report.cooked; break;
                    case CookOutcome::current: ++s.report.current; break;
                    case CookOutcome::failed:
                        ++s.report.failed;
                        s.report.errors.push_back(std::move(error));
                        break;
                    }
                }
                if (outcome == CookOutcome::failed) {
                    s.failed.fetch_add(1, std::memory_order_relaxed);
                }
                s.finished.fetch_add(1, std::memory_order_release);
            }
            {
                const std::scoped_lock lock{s.mutex};
                s.report.seconds = seconds_since(s.start);
            }
            s.done.store(true, std::memory_order_release);
        },
        &state.counter);
}

BackgroundCook::~BackgroundCook() {
    state_->cancelled.store(true, std::memory_order_relaxed);
    jobs_->wait(state_->counter);
}

bool BackgroundCook::done() const noexcept {
    return state_->done.load(std::memory_order_acquire);
}

CookProgress BackgroundCook::progress() const noexcept {
    return {.total = state_->items.size(),
            .finished = state_->finished.load(std::memory_order_acquire),
            .failed = state_->failed.load(std::memory_order_relaxed)};
}

void BackgroundCook::cancel() noexcept {
    state_->cancelled.store(true, std::memory_order_relaxed);
}

CookReport BackgroundCook::report() const {
    const std::scoped_lock lock{state_->mutex};
    return state_->report;
}

} // namespace ngine::cook
