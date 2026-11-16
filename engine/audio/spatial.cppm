module;

export module ngine.audio:spatial;

import ngine.core;
import ngine.math;
import :types;

// 3D-Klang (AUD-02) als reine Rechnung ohne Gerät: Abschwächung, Panning und Doppler. Der
// Hauptthread rechnet daraus pro Bild die Lautstärken links und rechts und die Tonhöhe jeder
// Stimme; der Mixer blendet nur noch weich dorthin.
export namespace ngine::audio {

// Schallgeschwindigkeit in Luft in m/s.
inline constexpr f32 speed_of_sound = 343.0f;

// Der Hörer: Lage in Weltkoordinaten (64 Bit), Achsen (x rechts, y oben, z hinten, wie die
// Kamera) und Geschwindigkeit.
struct ListenerPose {
    math::DVec3 position{0.0, 0.0, 0.0};
    math::Vec3 right{1.0f, 0.0f, 0.0f};
    math::Vec3 up{0.0f, 1.0f, 0.0f};
    math::Vec3 back{0.0f, 0.0f, 1.0f};
    math::Vec3 velocity{0.0f, 0.0f, 0.0f};
};

// Eine Quelle im Raum.
struct EmitterPose {
    math::DVec3 position{0.0, 0.0, 0.0};
    math::Vec3 velocity{0.0f, 0.0f, 0.0f};
    f32 min_distance = 1.0f;
    f32 max_distance = 50.0f;
    Rolloff rolloff = Rolloff::Inverse;
    f32 rolloff_factor = 1.0f;
    // 0 schaltet Doppler ab, 1 ist physikalisch.
    f32 doppler = 1.0f;
};

struct SpatialResult {
    // Lautstärke links und rechts (Abschwächung mal Panning).
    f32 left = 1.0f;
    f32 right = 1.0f;
    // Nur die Abschwächung (0 bis 1), für die Auswahl hörbarer Stimmen.
    f32 attenuation = 1.0f;
    // -1 ganz links, 1 ganz rechts.
    f32 pan = 0.0f;
    // Faktor der Tonhöhe durch Doppler.
    f32 doppler_pitch = 1.0f;
    f32 distance = 0.0f;
};

// Abschwächung im Abstand distance (siehe Rolloff). Unter min_distance 1, über max_distance der
// Wert bei max_distance.
[[nodiscard]] f32 attenuation(f32 distance, f32 min_distance, f32 max_distance, Rolloff rolloff,
                              f32 factor) noexcept;

// Lautstärke links und rechts mit gleicher Leistung: pan -1 nur links, 0 beide 0,707, 1 nur rechts.
struct PanGains {
    f32 left = 0.0f;
    f32 right = 0.0f;
};
[[nodiscard]] PanGains equal_power_pan(f32 pan) noexcept;

// Faktor der Tonhöhe durch Doppler. direction zeigt vom Hörer zur Quelle (Länge 1).
[[nodiscard]] f32 doppler_pitch(math::Vec3 direction, math::Vec3 listener_velocity,
                                math::Vec3 emitter_velocity, f32 factor) noexcept;

// Alles zusammen. Der Abstand entsteht in 64 Bit, damit 3D-Klang auch weit vom Ursprung stimmt
// (ADR 0012).
[[nodiscard]] SpatialResult spatialize(const ListenerPose& listener,
                                       const EmitterPose& emitter) noexcept;

// Hörer aus einer Weltlage (z. B. WorldTransform der Kamera); Skalierung zählt nicht.
[[nodiscard]] ListenerPose listener_from(const math::Affine& transform,
                                         math::Vec3 velocity = {0.0f, 0.0f, 0.0f}) noexcept;

} // namespace ngine::audio
