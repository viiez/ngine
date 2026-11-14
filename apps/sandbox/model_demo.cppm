module;

#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Standardansicht der Sandbox offscreen als Bild (--image). Die Szene selbst liegt seit R2.1 in
// apps/common (app.model_scene), weil auch der Editor sie zeigt.
export module sandbox.model_demo;

import ngine.core;
import ngine.filesystem;
import ngine.math;
import ngine.render;
import ngine.rhi;
import ngine.rhi.vulkan;
import app.model_scene;
import app.player;
import app.paths;
import ngine.asset;

namespace sandbox {

using namespace ngine;
using app::ModelScene;
using math::DVec3;

namespace {

rhi::Result<void> write_ppm(const std::string& path, std::span<const u8> rgba,
                            rhi::Extent2D extent) {
    const std::string header = std::format("P6\n{} {}\n255\n", extent.width, extent.height);
    std::vector<std::byte> data;
    data.reserve(header.size() + usize{extent.width} * extent.height * 3);
    for (const char c : header) {
        data.push_back(static_cast<std::byte>(c));
    }
    for (usize i = 0; i + 3 < rgba.size(); i += 4) {
        data.push_back(static_cast<std::byte>(rgba[i]));
        data.push_back(static_cast<std::byte>(rgba[i + 1]));
        data.push_back(static_cast<std::byte>(rgba[i + 2]));
    }
    return filesystem::write_file(path, data);
}

} // namespace

// Asset-Ordner und Cache gekochter Modelle der Sandbox (R2.5). Die Sandbox kocht nicht selbst
// (Kochen gehört in Editor und ngine_cook); fehlt der Cache, lädt sie aus den Quellen.
export class SandboxAssets {
public:
    explicit SandboxAssets(bool no_cache) {
        const auto root = app::find_upwards("assets");
        if (!root) {
            log::info("Kein Asset-Ordner gefunden; Standardszene ohne Assets");
            return;
        }
        auto opened = asset::AssetDatabase::open(*root);
        if (!opened) {
            log::warn("Asset-Ordner {} lässt sich nicht öffnen: {}", *root, opened.error());
            return;
        }
        database_.emplace(std::move(*opened));
        if (!no_cache) {
            cache_.emplace(asset::default_cache_dir(database_->root()));
        }
        log::info("Asset-Ordner: {} ({} Assets, Cache {})", database_->root(),
                  database_->assets().size(), no_cache ? "aus" : cache_->directory());
    }

    [[nodiscard]] app::SceneAssets scene_assets() const {
        return {.database = database_ ? &*database_ : nullptr,
                .loading = {.cache = cache_ ? &*cache_ : nullptr, .cook = {}}};
    }

    [[nodiscard]] const asset::AssetDatabase* database() const {
        return database_ ? &*database_ : nullptr;
    }

private:
    std::optional<asset::AssetDatabase> database_;
    std::optional<asset::AssetCache> cache_;
};

// Meldet, wie lange die Modelle der Szene zum Laden brauchten (Messung für die Abnahme von R2.5).
export void report_load(const app::LoadReport& report) {
    log::info("Modelle der Szene geladen in {:.1f} ms ({} Modelle, {} aus dem Cache, {} gekocht, "
              "{} aus Quellen, {} fehlgeschlagen)",
              report.milliseconds, report.models, report.counts.cached, report.counts.cooked,
              report.counts.source, report.failed);
}

// Offscreen auf der GPU zeichnen und als PPM speichern (für die CI): Kugelraster oder Modell.
export int run_model_offscreen(const std::optional<std::string>& model_path,
                               const std::string& image_path, bool baseline, DVec3 origin,
                               bool debug_draw, bool no_cache, bool playground = false) {
    auto device = rhi::vulkan::create_device({
        .application_name = "NGine Sandbox",
#ifdef NDEBUG
        .enable_validation = false,
#else
        .enable_validation = true,
#endif
        .disabled_features = {.mesh_shader = baseline, .ray_tracing = baseline},
    });
    if (!device) {
        log::fatal("{}", device.error());
        return 1;
    }
    int exit_code = 0;
    {
        constexpr rhi::Extent2D extent{960, 540};
        constexpr rhi::Format format = rhi::Format::RGBA8Srgb;
        auto target = (*device)->create_texture({.extent = extent, .format = format});
        if (!target) {
            log::fatal("{}", target.error());
            return 1;
        }
        SandboxAssets assets{no_cache};
        auto scene = playground ? ModelScene::create_playground(**device, format, origin,
                                                                assets.scene_assets())
                                : ModelScene::create(**device, format, model_path, origin,
                                                     app::SceneMode::Game, assets.scene_assets());
        if (!scene) {
            log::fatal("{}", scene.error());
            return 1;
        }
        (*scene)->set_debug_draw(debug_draw);
        report_load((*scene)->load_all(**device));
        if (playground) {
            // Zwei Sekunden Physik: Kugeln rollen die Rampe hinab, Kollisionsformen sichtbar.
            (*scene)->set_draw_colliders(true);
            (*scene)->physics().set_wait_for_terrain(true);
            // Schulterkamera leicht von oben, damit die Figur im Bild ist.
            if (auto* input = (*scene)->active_player().get_mut<app::PlayerInput>()) {
                input->view = app::CameraView::Shoulder;
                input->pitch = -0.3f;
            }
            for (u32 i = 0; i < 120; ++i) {
                (*scene)->wait_for_terrain();
                (*scene)->update(1.0f / 60.0f, {});
            }
            log::info("{}", (*scene)->character_stats());
            log::info("{}", (*scene)->physics_stats());
        }
        render::TexturePool pool{**device};
        // Das Terrain lädt seine Netze mit begrenztem Budget pro Bild hoch; der Spielplatz braucht
        // deshalb einige Bilder, bis alles zu sehen ist.
        const u32 frames = playground ? 30 : 1;
        for (u32 frame = 0; frame < frames; ++frame) {
            render::RenderGraph graph;
            const auto output = graph.import_texture(
                "Bild", **target, rhi::ResourceState::Undefined, rhi::ResourceState::TransferSrc);
            (*scene)->add_passes(graph, output);
            (*device)->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
            pool.end_frame();
        }
        log::info("Szene: {}", (*scene)->stats());
        const auto pixels = (*device)->read_texture(**target, rhi::ResourceState::TransferSrc);
        if (auto written = write_ppm(image_path, pixels, extent); !written) {
            log::error("Bild lässt sich nicht schreiben: {}", written.error());
            exit_code = 1;
        } else {
            log::info("Bild gespeichert: {}", image_path);
        }
    }
    if (const auto errors = (*device)->validation_errors(); errors > 0) {
        log::error("Die Validation Layers haben {} Fehler gemeldet", errors);
        exit_code = 1;
    }
    return exit_code;
}

} // namespace sandbox
