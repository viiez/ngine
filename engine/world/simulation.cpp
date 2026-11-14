module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

module ngine.world;

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.profile;

namespace ngine::world {

namespace {

// Spielraum beim Vergleich der angesammelten Zeit mit dem festen Schritt: Bildzeiten wie 1/30 s
// sind in f64 nicht exakt, und zwei Bilder zu 1/120 s sollen genau einen Schritt zu 1/60 s ergeben.
constexpr f64 time_epsilon = 1e-9;

bool same(const Transform& a, const Transform& b) noexcept {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.rotation.x == b.rotation.x &&
           a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
           a.rotation.w == b.rotation.w && a.scale.x == b.scale.x && a.scale.y == b.scale.y &&
           a.scale.z == b.scale.z;
}

Transform mix(const Transform& from, const Transform& to, f32 alpha) noexcept {
    return Transform{
        .position = math::lerp(from.position, to.position, static_cast<f64>(alpha)),
        .rotation = math::slerp(from.rotation, to.rotation, alpha),
        .scale = math::lerp(from.scale, to.scale, alpha),
    };
}

} // namespace

Simulation::Simulation(World& world, jobs::JobSystem& jobs, SimulationOptions options)
    : world_{&world}, jobs_{&jobs}, options_{options}, schedule_{world}, interpolated_{world} {
    verify(options_.fixed_step > 0.0, "Simulation: fester Schritt muss positiv sein");
    verify(options_.max_steps > 0, "Simulation: mindestens ein fester Schritt pro Bild");
    Time time = world.time();
    time.fixed_delta = static_cast<f32>(options_.fixed_step);
    world.set_time(time);
}

Simulation::~Simulation() = default;

void Simulation::set_paused(bool paused) noexcept {
    if (paused_ && !paused) {
        // Nach der Pause gleich mit einem Schritt weitermachen: In der Pause zeigt das Bild die
        // neueste Lage (alpha = 1); so geht es ohne Sprung zurück weiter.
        accumulator_ = options_.fixed_step;
    }
    paused_ = paused;
}

void Simulation::set_time_scale(f64 scale) noexcept {
    time_scale_ = std::max(scale, 0.0);
}

void Simulation::reset_clock() noexcept {
    accumulator_ = 0.0;
    step_requested_ = false;
    Time time;
    time.fixed_delta = static_cast<f32>(options_.fixed_step);
    time.paused = paused_;
    world_->set_time(time);
}

u32 Simulation::advance(f64 dt) {
    NGINE_PROFILE_ZONE("Simulation::advance");
    dt = std::max(dt, 0.0);
    const f64 fixed = options_.fixed_step;

    u32 steps = 0;
    if (paused_) {
        steps = step_requested_ ? 1 : 0;
    } else {
        accumulator_ += dt * time_scale_;
        while (accumulator_ + time_epsilon >= fixed && steps < options_.max_steps) {
            accumulator_ -= fixed;
            ++steps;
        }
        // Was über max_steps hinausgeht, verfällt; der Anteil innerhalb eines Schritts bleibt.
        if (accumulator_ >= fixed) {
            accumulator_ = std::fmod(accumulator_, fixed);
        }
        accumulator_ = std::max(accumulator_, 0.0);
        if (step_requested_ && steps == 0) {
            steps = 1;
        }
    }
    step_requested_ = false;

    Time time = world_->time();
    ++time.frame;
    time.real_delta = static_cast<f32>(dt);
    time.paused = paused_;
    world_->set_time(time);

    for (u32 i = 0; i < steps; ++i) {
        fixed_step();
    }

    time = world_->time();
    time.alpha = paused_ ? 1.0f : static_cast<f32>(std::clamp(accumulator_ / fixed, 0.0, 1.0));
    time.delta = paused_ ? 0.0f : static_cast<f32>(dt * time_scale_);
    world_->set_time(time);
    schedule_.run(*jobs_, Phase::Frame);
    schedule_.run(*jobs_, Phase::Late);
    interpolate();
    return steps;
}

void Simulation::fixed_step() {
    NGINE_PROFILE_ZONE("Simulation::fixed_step");
    // Lage vor dem Schritt merken, zwischen ihr und der neuen wird gemischt.
    interpolated_.each([](Interpolated& interpolated, const Transform& transform) {
        interpolated.previous = transform;
        interpolated.initialized = true;
    });

    Time time = world_->time();
    ++time.tick;
    time.elapsed = static_cast<f64>(time.tick) * options_.fixed_step;
    time.delta = time.fixed_delta;
    world_->set_time(time);
    schedule_.run(*jobs_, Phase::Fixed);
    for (const auto& stage : fixed_stages_) {
        stage();
    }
    if (after_fixed_step_) {
        after_fixed_step_();
    }
}

void Simulation::interpolate() {
    NGINE_PROFILE_ZONE("Simulation::interpolate");
    // Für das Zeichnen hält Transform kurz die gemischte Lage: update_transforms() rechnet daraus
    // WorldTransform, Kinder ziehen über die Hierarchie mit. Danach kommt die Lage der Simulation
    // zurück, ohne die Änderungserkennung zu wecken: Speichern, Replikation und der nächste
    // Schritt sehen die Lage der Simulation, ein späterer Aufruf von update_transforms() lässt die
    // gemischte WorldTransform stehen. Nur Entities, die sich im letzten Schritt bewegt haben,
    // werden angefasst; ruhende lösen keine Neuberechnung aus.
    const f32 alpha = world_->time().alpha;
    std::vector<u64> moving;
    std::vector<u64> settled;
    interpolated_.each([&](Entity entity, Interpolated& interpolated, const Transform& transform) {
        if (!interpolated.initialized) {
            interpolated.previous = transform;
            interpolated.initialized = true;
        }
        if (alpha < 1.0f && !same(interpolated.previous, transform)) {
            interpolated.simulated = transform;
            interpolated.mixed = true;
            moving.push_back(entity.id());
        } else if (interpolated.mixed) {
            // Zum Stehen gekommen: WorldTransform hält noch eine gemischte Lage.
            interpolated.mixed = false;
            settled.push_back(entity.id());
        }
    });

    const ComponentId transform_id = world_->component<Transform>();
    const ComponentId interpolated_id = world_->component<Interpolated>();
    for (const u64 entity : settled) {
        world_->modified(entity, transform_id);
    }
    for (const u64 entity : moving) {
        const auto* interpolated =
            static_cast<const Interpolated*>(world_->get(entity, interpolated_id));
        auto* transform = static_cast<Transform*>(world_->get_mut(entity, transform_id));
        *transform = mix(interpolated->previous, interpolated->simulated, alpha);
        world_->modified(entity, transform_id);
    }
    (void)update_transforms(*world_);
    for (const u64 entity : moving) {
        const auto* interpolated =
            static_cast<const Interpolated*>(world_->get(entity, interpolated_id));
        // Ohne modified(): siehe oben.
        *static_cast<Transform*>(world_->get_mut(entity, transform_id)) = interpolated->simulated;
    }
}

} // namespace ngine::world
