module;

#include <algorithm>
#include <cmath>
#include <numbers>

module ngine.audio;

import ngine.core;
import ngine.math;

namespace ngine::audio {

f32 attenuation(f32 distance, f32 min_distance, f32 max_distance, Rolloff rolloff,
                f32 factor) noexcept {
    const f32 lo = std::max(min_distance, 0.001f);
    const f32 hi = std::max(max_distance, lo);
    if (distance >= hi) {
        // Ab max_distance still, bei jeder Kurve (Abnahme R4.5: sonst blieb ein Rest hörbar).
        return 0.0f;
    }
    const f32 d = std::clamp(distance, lo, hi);
    f32 gain = 1.0f;
    switch (rolloff) {
    case Rolloff::Inverse: gain = lo / (lo + std::max(factor, 0.0f) * (d - lo)); break;
    case Rolloff::Linear:
        if (hi <= lo) {
            return 1.0f;
        }
        return std::clamp(1.0f - std::max(factor, 0.0f) * (d - lo) / (hi - lo), 0.0f, 1.0f);
    case Rolloff::Exponential: gain = std::pow(d / lo, -std::max(factor, 0.0f)); break;
    }
    // Inverse und Exponential laufen im letzten Fünftel bis max_distance weich auf null aus,
    // statt am Rand abzubrechen.
    const f32 fade_start = hi - (hi - lo) * max_distance_fade;
    if (d > fade_start) {
        const f32 t = (hi - d) / (hi - fade_start);
        gain *= t * t * (3.0f - 2.0f * t);
    }
    return gain;
}

PanGains equal_power_pan(f32 pan) noexcept {
    const f32 angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * (std::numbers::pi_v<f32> * 0.25f);
    return PanGains{.left = std::cos(angle), .right = std::sin(angle)};
}

f32 doppler_pitch(math::Vec3 direction, math::Vec3 listener_velocity, math::Vec3 emitter_velocity,
                  f32 factor) noexcept {
    if (factor <= 0.0f) {
        return 1.0f;
    }
    // Geschwindigkeiten entlang der Verbindung, begrenzt unter die Schallgeschwindigkeit.
    const f32 limit = speed_of_sound * 0.5f;
    const f32 listener =
        std::clamp(math::dot(listener_velocity, direction) * factor, -limit, limit);
    const f32 emitter = std::clamp(math::dot(emitter_velocity, direction) * factor, -limit, limit);
    const f32 pitch = (speed_of_sound + listener) / (speed_of_sound + emitter);
    return std::clamp(pitch, 0.5f, 2.0f);
}

SpatialResult spatialize(const ListenerPose& listener, const EmitterPose& emitter) noexcept {
    // Differenz in 64 Bit, erst das kleine Ergebnis in 32 Bit.
    const math::Vec3 offset = math::to_f32(emitter.position - listener.position);
    const f32 distance = math::length(offset);

    SpatialResult result;
    result.distance = distance;
    result.attenuation = attenuation(distance, emitter.min_distance, emitter.max_distance,
                                     emitter.rolloff, emitter.rolloff_factor);

    // Seitlicher Anteil der Richtung im Raum des Hörers. Ganz nah am Kopf läuft das Panning zur
    // Mitte aus, damit eine Quelle beim Durchgehen nicht von links nach rechts springt.
    constexpr f32 near = 0.25f;
    if (distance > 1e-4f) {
        const math::Vec3 direction = offset / distance;
        const f32 side = math::dot(direction, listener.right);
        result.pan = std::clamp(side, -1.0f, 1.0f) * std::min(distance / near, 1.0f);
        result.doppler_pitch =
            doppler_pitch(direction, listener.velocity, emitter.velocity, emitter.doppler);
    }
    const PanGains gains = equal_power_pan(result.pan);
    result.left = gains.left * result.attenuation;
    result.right = gains.right * result.attenuation;
    return result;
}

ListenerPose listener_from(const math::Affine& transform, math::Vec3 velocity) noexcept {
    const auto axis = [](math::Vec3 v, math::Vec3 fallback) {
        const f32 len = math::length(v);
        return len > 1e-6f ? v / len : fallback;
    };
    return ListenerPose{.position = transform.translation,
                        .right = axis(transform.linear[0], {1.0f, 0.0f, 0.0f}),
                        .up = axis(transform.linear[1], {0.0f, 1.0f, 0.0f}),
                        .back = axis(transform.linear[2], {0.0f, 0.0f, 1.0f}),
                        .velocity = velocity};
}

math::Vec3 MotionTracker::update(math::DVec3 position, f64 dt) noexcept {
    if (!have_) {
        last_ = position;
        have_ = true;
        return smoothed_;
    }
    if (dt <= 0.0) {
        return smoothed_;
    }
    since_ += dt;
    if (position != last_) {
        // Weg seit der letzten Änderung durch die Zeit seitdem: gleich, ob die Lage in jedem Bild
        // oder nur in jedem zweiten Bild weiterrückt.
        raw_ = math::to_f32((position - last_) / since_);
        last_ = position;
        since_ = 0.0;
    } else if (since_ > hold_seconds) {
        raw_ = {0.0f, 0.0f, 0.0f};
    }
    // Sprünge (z. B. Teleport) nicht als Überschallflug werten.
    if (math::length(raw_) > speed_of_sound) {
        raw_ = {0.0f, 0.0f, 0.0f};
    }
    const auto alpha = static_cast<f32>(1.0 - std::exp(-dt / smoothing_seconds));
    smoothed_ = smoothed_ + (raw_ - smoothed_) * alpha;
    return smoothed_;
}

} // namespace ngine::audio
