module;

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

// Messung vieler gleichzeitiger Klangquellen (--audio-voices <n>, R4.5): n geortete Quellen mit
// einem Summen in Schleife kreisen um den Hörer, das AudioSystem rechnet pro Bild Abschwächung,
// Panning und Doppler, der Mixer mischt im eigenen Thread (Gerät des Systems oder mit --headless
// bzw. --no-audio das Null-Gerät im Echtzeit-Takt). Gemeldet werden die Zeit des AudioSystem pro
// Bild und Last, Leerläufe und Stimmen des Mixers.
export module sandbox.audio_bench;

import ngine.core;
import ngine.asset;
import ngine.audio;
import ngine.math;
import ngine.reflect;
import ngine.world;

namespace sandbox {

using namespace ngine;

// Summen mit Obertönen über seconds Sekunden (bei ganzen Sekunden nahtlos in Schleife), erzeugt
// statt aus dem Asset-Ordner, z. B. für Messungen und den Spielplatz ohne Fenster.
export audio::Sound generated_hum(u32 rate, f64 seconds = 2.0) {
    auto data = std::make_shared<audio::SoundData>();
    data->channels = 1;
    data->sample_rate = rate;
    const auto frames = static_cast<u32>(static_cast<f64>(rate) * seconds);
    data->samples.resize(frames);
    for (u32 i = 0; i < frames; ++i) {
        const f64 p = 2.0 * std::numbers::pi * 165.0 * static_cast<f64>(i) / static_cast<f64>(rate);
        data->samples[i] =
            static_cast<f32>(0.4 * std::sin(p) + 0.2 * std::sin(2.0 * p) + 0.1 * std::sin(3.0 * p));
    }
    audio::Sound sound;
    sound.info = {.channels = 1, .sample_rate = rate, .frames = frames};
    sound.data = std::move(data);
    return sound;
}

namespace {

f64 percentile(std::vector<f64> values, f64 p) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto index = static_cast<usize>(p * static_cast<f64>(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

} // namespace

export int run_audio_bench(u32 voices, u64 frames, bool use_device) {
    audio::AudioEngine engine{
        {.mode = use_device ? audio::DeviceMode::Automatic : audio::DeviceMode::Null}};
    log::info("Audio-Messung: {} Quellen, {} Bilder, Gerät {}", voices, frames,
              engine.device().name());
    // Leise, damit viele Quellen auf dem echten Gerät nicht dröhnen.
    engine.set_bus_volume(audio::AudioBus::Master, 0.2f);

    reflect::TypeRegistry types;
    world::register_types(types);
    audio::register_types(types);
    world::World world{types};
    asset::ResourceManager resources{nullptr, nullptr};
    constexpr u64 hum_id = asset::generated_id("sandbox.audio_bench.hum");
    static_cast<void>(resources.add(hum_id, generated_hum(engine.sample_rate())));
    audio::AudioSystem system{engine, resources};
    system.set_fallback_listener(
        math::Affine::from({0.0, 0.0, 0.0}, math::quat_identity(), {1.0f, 1.0f, 1.0f}));

    struct Emitter {
        world::Entity entity;
        f64 radius;
        f64 angle;
        f64 speed;
    };
    std::vector<Emitter> emitters;
    emitters.reserve(voices);
    for (u32 i = 0; i < voices; ++i) {
        // Goldener Winkel: gleichmäßig verteilt, Abstände von 2 bis gut 60 m.
        const f64 angle = static_cast<f64>(i) * 2.399963;
        const f64 radius = 2.0 + 2.7 * std::sqrt(static_cast<f64>(i));
        world::Entity entity = world.create(std::format("Quelle {}", i + 1));
        entity
            .set(world::Transform{
                .position = {std::cos(angle) * radius, 1.0, std::sin(angle) * radius}})
            .set(audio::AudioSource{.sound = hum_id,
                                    .volume = 0.5f,
                                    .pitch = 0.8f + 0.4f * static_cast<f32>(i % 7) / 6.0f,
                                    .loop = true,
                                    .max_distance = 80.0f});
        emitters.push_back({.entity = entity,
                            .radius = radius,
                            .angle = angle,
                            .speed = (i % 2 == 0 ? 1.0 : -1.0) * 4.0 / radius});
    }

    constexpr f64 dt = 1.0 / 60.0;
    std::vector<f64> update_ms;
    update_ms.reserve(frames);
    auto next = std::chrono::steady_clock::now();
    for (u64 frame = 0; frame < frames; ++frame) {
        for (Emitter& emitter : emitters) {
            emitter.angle += emitter.speed * dt;
            emitter.entity.get_mut<world::Transform>()->position = {
                std::cos(emitter.angle) * emitter.radius, 1.0,
                std::sin(emitter.angle) * emitter.radius};
        }
        (void)world::update_transforms(world);
        const auto start = std::chrono::steady_clock::now();
        system.update(world, dt);
        engine.update();
        update_ms.push_back(
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
        // Im Takt von 60 Bildern pro Sekunde, damit der Mixer in Echtzeit mitläuft.
        next += std::chrono::microseconds{16667};
        std::this_thread::sleep_until(next);
    }

    const audio::AudioStats stats = engine.stats();
    const audio::AudioSystemStats system_stats = system.stats();
    log::info("AudioSystem pro Bild: Median {:.3f} ms, 99 % {:.3f} ms, höchstens {:.3f} ms",
              percentile(update_ms, 0.5), percentile(update_ms, 0.99), percentile(update_ms, 1.0));
    log::info("Mixer: {} Stimmen ({} hörbar, {} virtuell), Block zuletzt {:.3f} ms, höchstens "
              "{:.3f} ms, Last {:.0f} %, {} Leerläufe, {} verworfene Befehle",
              stats.mixer.active, stats.mixer.audible, stats.mixer.virtual_voices,
              stats.mixer.last_block_ms, stats.mixer.max_block_ms, stats.mixer.load * 100.0,
              stats.mixer.underruns, stats.mixer.dropped_commands);
    log::info("Quellen: {} in der Welt, {} spielen", system_stats.sources, system_stats.playing);
    const bool ok = system_stats.playing == std::min<usize>(voices, audio::max_voices) &&
                    stats.mixer.underruns == 0;
    if (!ok) {
        log::error("Audio-Messung: nicht alle Quellen spielen oder Leerläufe");
    }
    system.stop_all();
    return ok ? 0 : 1;
}

} // namespace sandbox
