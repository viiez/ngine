module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

// Spielplatz der Physik ohne Fenster und GPU (--playground --headless, R4.2): dieselbe Welt wie in
// der Sandbox mit Fenster, gerechnet über die Spielschleife. Meldet am Ende eine Prüfsumme der
// Lagen (gleiche Szene, gleicher Build: gleiche Summe) und mit --bodies die Zeiten von Physik und
// update_transforms().
export module sandbox.playground;

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.physics;
import ngine.profile;
import ngine.reflect;
import ngine.render;
import ngine.world;
import app.physics;

namespace sandbox {

using namespace ngine;

namespace {

struct Timing {
    std::vector<f64> values;

    void add(f64 value) {
        values.push_back(value);
    }
    [[nodiscard]] f64 percentile(f64 p) {
        if (values.empty()) {
            return 0.0;
        }
        std::ranges::sort(values);
        const auto index = static_cast<usize>(p * static_cast<f64>(values.size() - 1) + 0.5);
        return values[std::min(index, values.size() - 1)];
    }
};

} // namespace

export int run_playground_headless(u64 frames, math::DVec3 origin, u32 extra_bodies) {
    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    physics::register_types(types);
    world::World world{types};
    app::build_playground(world, origin);
    if (extra_bodies > 0) {
        app::rain_bodies(world, origin + math::DVec3{0.0, 0.0, 14.0}, extra_bodies);
    }
    jobs::JobSystem jobs;
    world::Simulation simulation{world, jobs};
    app::ModelShapes shapes;
    shapes.add(app::playground_id, app::playground_model());
    physics::PhysicsWorld physics{world, jobs};
    physics.set_shape_source(shapes.source());
    physics::add_to(simulation, physics);

    // Ein Bild ist 1/60 s, also ein fester Schritt pro Bild, so schnell es geht.
    Timing step;
    Timing sync;
    Timing write_back;
    Timing transforms;
    const auto start = std::chrono::steady_clock::now();
    for (u64 frame = 0; frame < frames; ++frame) {
        NGINE_PROFILE_ZONE("Frame");
        (void)simulation.advance(1.0 / 60.0);
        const physics::StepStats& stats = physics.stats();
        step.add(stats.sync_ms + stats.update_ms + stats.write_back_ms);
        sync.add(stats.sync_ms);
        write_back.add(stats.write_back_ms);
        transforms.add(simulation.stats().transforms_ms);
        profile::frame_mark();
    }
    const std::chrono::duration<f64, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    log::info(
        "Spielplatz: {} feste Schritte ({:.2f} s simuliert) in {:.0f} ms mit {} Worker-Threads",
        simulation.time().tick, simulation.time().elapsed, elapsed.count(), jobs.worker_count());
    log::info("Physik: {} Körper, am Ende {} wach; Schritt Median {:.2f} ms, 99. Perzentil {:.2f} "
              "ms (Abgleich Median {:.2f} ms, Rückschrieb Median {:.2f} ms); update_transforms "
              "Median {:.2f} ms, 99. Perzentil {:.2f} ms",
              physics.body_count(), physics.stats().active_bodies, step.percentile(0.5),
              step.percentile(0.99), sync.percentile(0.5), write_back.percentile(0.5),
              transforms.percentile(0.5), transforms.percentile(0.99));
    log::info("Prüfsumme der Lagen: {:016x}", physics::checksum(world));
    return 0;
}

} // namespace sandbox
