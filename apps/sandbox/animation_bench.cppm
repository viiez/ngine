module;

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Messung vieler animierter Figuren ohne Fenster (--animators <n> --headless, R4.6): n Figuren mit
// der Puppe und dem Automaten der Spielfigur aus dem Asset-Ordner, mit verschiedenem Tempo und
// verschiedener Geschwindigkeit, rechnen Automat, Pose und Palette (pose_mode Always) einmal im
// Hauptthread und einmal in Jobs. Gemeldet werden die Zeiten des AnimationSystem pro Bild und der
// Speicher der Paletten. Die Zeit des Skinning-Passes auf der GPU misst --playground --animators
// <n> im Fenster (GPU-Zeiten je Pass im Log).
export module sandbox.animation_bench;

import ngine.core;
import ngine.animation;
import ngine.asset;
import ngine.jobs;
import ngine.math;
import ngine.reflect;
import ngine.world;
import app.paths;

namespace sandbox {

using namespace ngine;

namespace {

f64 percentile(std::vector<f64> values, f64 p) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto index = static_cast<usize>(p * static_cast<f64>(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

struct Run {
    f64 median = 0.0;
    f64 p99 = 0.0;
    usize posed = 0;
    usize palette_bytes = 0;
};

Run measure(asset::ResourceManager& resources, jobs::JobSystem* jobs, u64 graph, u64 model,
            u32 count, u64 frames) {
    reflect::TypeRegistry types;
    world::register_types(types);
    animation::register_types(types);
    world::World world{types};
    animation::AnimationSystem system{resources, jobs};
    system.set_rendering(false);
    std::vector<u64> figures;
    for (u32 i = 0; i < count; ++i) {
        const f32 phase = static_cast<f32>(i % 17) / 17.0f;
        figures.push_back(
            world.create(std::format("Figur {}", i + 1))
                .set(world::Transform{.position = {static_cast<f64>(i % 20) * 1.5, 0.0,
                                                   static_cast<f64>(i / 20) * 1.5}})
                .set(animation::Animator{.graph = graph,
                                         .speed = 0.8f + 0.4f * phase,
                                         .pose_mode = animation::PoseMode::Always})
                .id());
        // Stehen, Gehen und Laufen gemischt, damit Mischungen verschieden sind.
        system.set_float(figures.back(), "Tempo", 7.0f * phase);
        system.set_bool(figures.back(), "AmBoden", true);
    }
    // Bis Automaten und Rig geladen sind, dann messen.
    for (u32 i = 0; i < 600 && system.stats().running < count; ++i) {
        resources.wait();
        resources.update();
        system.update(world, 1.0 / 60.0);
    }
    std::vector<f64> times;
    for (u64 frame = 0; frame < frames; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        system.update(world, 1.0 / 60.0);
        times.push_back(
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
    }
    Run run{.median = percentile(times, 0.5),
            .p99 = percentile(times, 0.99),
            .posed = system.stats().posed};
    for (const u64 figure : figures) {
        run.palette_bytes += system.palette(world, figure, model, 0).size_bytes();
    }
    return run;
}

} // namespace

export int run_animation_bench(u32 count, u64 frames) {
    const auto root = app::find_upwards("assets", "figuren/spielfigur.animgraph");
    if (!root) {
        log::error("Animation: kein Asset-Ordner mit figuren/spielfigur.animgraph");
        return 1;
    }
    auto database = asset::AssetDatabase::open(*root);
    if (!database) {
        log::error("Animation: {}", database.error());
        return 1;
    }
    const asset::AssetInfo* graph = database->find_path("figuren/spielfigur.animgraph");
    const asset::AssetInfo* model = database->find_path("figuren/puppe.gltf");
    if (graph == nullptr || model == nullptr) {
        log::error("Animation: Puppe oder Automat fehlen");
        return 1;
    }
    jobs::JobSystem jobs;
    asset::ResourceManager resources{&*database, &jobs};
    animation::register_loaders(resources);
    animation::register_graph_loader(resources);
    log::info("Animations-Messung: {} Figuren, {} Bilder, {} Worker-Threads", count, frames,
              jobs.worker_count());
    const Run serial = measure(resources, nullptr, graph->id, model->id, count, frames);
    const Run parallel = measure(resources, &jobs, graph->id, model->id, count, frames);
    log::info("AnimationSystem im Hauptthread: Median {:.3f} ms, 99 % {:.3f} ms, {} Posen",
              serial.median, serial.p99, serial.posed);
    log::info("AnimationSystem in Jobs: Median {:.3f} ms, 99 % {:.3f} ms, {} Posen",
              parallel.median, parallel.p99, parallel.posed);
    log::info("Paletten: {:.1f} KiB für {} Figuren ({} Bytes je Figur)",
              static_cast<f64>(parallel.palette_bytes) / 1024.0, count,
              count > 0 ? parallel.palette_bytes / count : 0);
    return serial.posed == count && parallel.posed == count ? 0 : 1;
}

} // namespace sandbox
