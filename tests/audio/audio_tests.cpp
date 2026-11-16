#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.asset;
import ngine.world;
import ngine.audio;

using namespace ngine;
using Catch::Matchers::WithinAbs;

namespace {

// Sinuston als Klang im Speicher.
audio::SoundData sine(u32 rate, f64 seconds, f32 frequency = 440.0f, u32 channels = 1,
                      f32 amplitude = 0.5f) {
    audio::SoundData data;
    data.channels = channels;
    data.sample_rate = rate;
    const auto frames = static_cast<usize>(seconds * rate);
    data.samples.resize(frames * channels);
    for (usize i = 0; i < frames; ++i) {
        const f32 value = amplitude * std::sin(2.0f * std::numbers::pi_v<f32> * frequency *
                                               static_cast<f32>(i) / static_cast<f32>(rate));
        for (u32 c = 0; c < channels; ++c) {
            data.samples[i * channels + c] = value;
        }
    }
    return data;
}

audio::Sound memory_sound(audio::SoundData data) {
    audio::Sound sound;
    sound.info = audio::SoundInfo{
        .channels = data.channels, .sample_rate = data.sample_rate, .frames = data.frames()};
    sound.data = std::make_shared<const audio::SoundData>(std::move(data));
    return sound;
}

struct Levels {
    f32 left = 0.0f;
    f32 right = 0.0f;
};

// Mittlere Leistung (RMS) links und rechts.
Levels rms(std::span<const f32> samples) {
    f64 l = 0.0;
    f64 r = 0.0;
    const usize frames = samples.size() / 2;
    for (usize i = 0; i < frames; ++i) {
        l += static_cast<f64>(samples[i * 2]) * samples[i * 2];
        r += static_cast<f64>(samples[i * 2 + 1]) * samples[i * 2 + 1];
    }
    if (frames == 0) {
        return {};
    }
    return Levels{static_cast<f32>(std::sqrt(l / static_cast<f64>(frames))),
                  static_cast<f32>(std::sqrt(r / static_cast<f64>(frames)))};
}

// Mischt so lange, bis die Stimme zu Ende ist; liefert die Zahl der Frames.
u64 render_until_finished(audio::AudioEngine& engine, audio::Voice voice, u32 block, u64 limit) {
    u64 frames = 0;
    while (frames < limit) {
        engine.manual_device()->render(block);
        frames += block;
        engine.update();
        if (!engine.playing(voice)) {
            break;
        }
    }
    return frames;
}

// Schreibt eine WAV-Datei (16 Bit PCM).
void write_wav(const std::filesystem::path& path, const audio::SoundData& data) {
    std::ofstream out{path, std::ios::binary};
    const auto put32 = [&](u32 v) {
        const char bytes[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                               static_cast<char>((v >> 16) & 0xFF),
                               static_cast<char>((v >> 24) & 0xFF)};
        out.write(bytes, 4);
    };
    const auto put16 = [&](u16 v) {
        const char bytes[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
        out.write(bytes, 2);
    };
    const auto bytes = static_cast<u32>(data.samples.size() * 2);
    out.write("RIFF", 4);
    put32(36 + bytes);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(static_cast<u16>(data.channels));
    put32(data.sample_rate);
    put32(data.sample_rate * data.channels * 2);
    put16(static_cast<u16>(data.channels * 2));
    put16(16);
    out.write("data", 4);
    put32(bytes);
    for (const f32 s : data.samples) {
        put16(static_cast<u16>(static_cast<i16>(std::clamp(s, -1.0f, 1.0f) * 32767.0f)));
    }
}

std::filesystem::path temp_dir() {
    const auto dir = std::filesystem::temp_directory_path() / "ngine_audio_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

audio::AudioOptions manual() {
    return audio::AudioOptions{.mode = audio::DeviceMode::Manual};
}

} // namespace

TEST_CASE("Attenuation curves", "[audio]") {
    using audio::Rolloff;
    CHECK(audio::attenuation(0.5f, 1.0f, 50.0f, Rolloff::Inverse, 1.0f) == 1.0f);
    CHECK_THAT(audio::attenuation(2.0f, 1.0f, 50.0f, Rolloff::Inverse, 1.0f), WithinAbs(0.5, 1e-6));
    CHECK_THAT(audio::attenuation(10.0f, 1.0f, 50.0f, Rolloff::Inverse, 1.0f),
               WithinAbs(0.1, 1e-6));
    // Über max_distance nicht mehr leiser.
    CHECK(audio::attenuation(500.0f, 1.0f, 50.0f, Rolloff::Inverse, 1.0f) ==
          audio::attenuation(50.0f, 1.0f, 50.0f, Rolloff::Inverse, 1.0f));
    CHECK_THAT(audio::attenuation(25.5f, 1.0f, 50.0f, Rolloff::Linear, 1.0f), WithinAbs(0.5, 1e-6));
    CHECK(audio::attenuation(50.0f, 1.0f, 50.0f, Rolloff::Linear, 1.0f) == 0.0f);
    CHECK_THAT(audio::attenuation(4.0f, 1.0f, 50.0f, Rolloff::Exponential, 1.0f),
               WithinAbs(0.25, 1e-6));
}

TEST_CASE("Equal power panning keeps the power constant", "[audio]") {
    for (const f32 pan : {-1.0f, -0.5f, 0.0f, 0.3f, 1.0f}) {
        const auto g = audio::equal_power_pan(pan);
        CHECK_THAT(g.left * g.left + g.right * g.right, WithinAbs(1.0, 1e-5));
    }
    CHECK_THAT(audio::equal_power_pan(-1.0f).right, WithinAbs(0.0, 1e-6));
    CHECK_THAT(audio::equal_power_pan(1.0f).left, WithinAbs(0.0, 1e-6));
    CHECK_THAT(audio::equal_power_pan(0.0f).left, WithinAbs(std::sqrt(0.5), 1e-6));
}

TEST_CASE("Spatialize places sources left and right, also far from the origin", "[audio]") {
    for (const f64 origin : {0.0, 1.0e7}) {
        // Hörer blickt entlang -z, rechts ist +x.
        const audio::ListenerPose listener{.position = {origin, 2.0, origin}};
        const auto right = audio::spatialize(
            listener, audio::EmitterPose{.position = {origin + 5.0, 2.0, origin}});
        const auto left = audio::spatialize(
            listener, audio::EmitterPose{.position = {origin - 5.0, 2.0, origin}});
        const auto ahead = audio::spatialize(
            listener, audio::EmitterPose{.position = {origin, 2.0, origin - 5.0}});
        CHECK(right.right > 0.19f);
        CHECK(right.left < 0.01f);
        CHECK(left.left > 0.19f);
        CHECK(left.right < 0.01f);
        CHECK_THAT(ahead.left, WithinAbs(ahead.right, 1e-5));
        CHECK_THAT(ahead.distance, WithinAbs(5.0, 1e-4));
        CHECK_THAT(ahead.attenuation, WithinAbs(0.2, 1e-5));
    }
    // Gedrehter Hörer: blickt entlang +x, dann liegt +z rechts.
    const auto turned = audio::listener_from(math::Affine::from(
        {0.0, 0.0, 0.0}, math::look_rotation({1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}),
        {1.0f, 1.0f, 1.0f}));
    const auto side = audio::spatialize(turned, audio::EmitterPose{.position = {0.0, 0.0, 5.0}});
    CHECK(side.right > side.left * 10.0f);
}

TEST_CASE("Doppler raises the pitch of approaching sources", "[audio]") {
    const audio::ListenerPose listener{};
    const audio::EmitterPose approaching{.position = {0.0, 0.0, -20.0},
                                         .velocity = {0.0f, 0.0f, 30.0f}};
    const audio::EmitterPose leaving{.position = {0.0, 0.0, -20.0},
                                     .velocity = {0.0f, 0.0f, -30.0f}};
    const f32 up = audio::spatialize(listener, approaching).doppler_pitch;
    const f32 down = audio::spatialize(listener, leaving).doppler_pitch;
    CHECK_THAT(up, WithinAbs(343.0 / (343.0 - 30.0), 1e-4));
    CHECK_THAT(down, WithinAbs(343.0 / (343.0 + 30.0), 1e-4));
    audio::EmitterPose off = approaching;
    off.doppler = 0.0f;
    CHECK(audio::spatialize(listener, off).doppler_pitch == 1.0f);
}

TEST_CASE("Lock-free queues pass items between threads", "[audio]") {
    audio::SpscQueue<u32, 64> queue;
    audio::SampleRing ring{1000};
    constexpr u32 count = 20000;
    std::thread producer{[&] {
        std::vector<f32> chunk(37);
        u32 next_ring = 0;
        for (u32 i = 0; i < count; ++i) {
            while (!queue.push(i)) {
                std::this_thread::yield();
            }
        }
        while (next_ring < count) {
            const u32 n = std::min<u32>(37, count - next_ring);
            for (u32 k = 0; k < n; ++k) {
                chunk[k] = static_cast<f32>(next_ring + k);
            }
            const usize written = ring.write(std::span<const f32>{chunk.data(), n});
            next_ring += static_cast<u32>(written);
            if (written == 0) {
                std::this_thread::yield();
            }
        }
    }};
    u32 expected = 0;
    bool ordered = true;
    while (expected < count) {
        u32 value = 0;
        if (queue.pop(value)) {
            ordered = ordered && value == expected;
            ++expected;
        }
    }
    std::vector<f32> out(53);
    u32 got = 0;
    while (got < count) {
        const usize n = ring.read(out);
        for (usize k = 0; k < n; ++k) {
            ordered = ordered && out[k] == static_cast<f32>(got + k);
        }
        got += static_cast<u32>(n);
    }
    producer.join();
    CHECK(ordered);
    CHECK(queue.size() == 0);
    CHECK(ring.available() == 0);
}

TEST_CASE("Mixer plays a sound and reports its end", "[audio]") {
    audio::AudioEngine engine{manual()};
    REQUIRE(engine.manual_device() != nullptr);
    const auto sound = memory_sound(sine(48000, 0.1));
    const audio::Voice voice = engine.play(sound, audio::PlayParams{});
    REQUIRE(voice.valid());
    CHECK(engine.playing(voice));

    const auto block = engine.manual_device()->render(1024);
    const Levels level = rms(block);
    CHECK(level.left > 0.2f);
    CHECK_THAT(level.left, WithinAbs(level.right, 1e-6));

    // 0,1 s bei 48 kHz: 4800 Frames, danach ist die Stimme frei.
    const u64 frames = render_until_finished(engine, voice, 256, 48000) + 1024;
    CHECK(frames >= 4800);
    CHECK(frames <= 4800 + 1024 + 256);
    CHECK_FALSE(engine.playing(voice));
    CHECK(engine.stats().voices == 0);
}

TEST_CASE("Mixer resamples to the device rate", "[audio]") {
    audio::AudioEngine engine{manual()};
    // 0,1 s bei 24 kHz dauern am Gerät mit 48 kHz ebenfalls 0,1 s = 4800 Frames.
    const auto sound = memory_sound(sine(24000, 0.1));
    const audio::Voice voice = engine.play(sound, {});
    const u64 frames = render_until_finished(engine, voice, 64, 48000);
    CHECK(frames >= 4800);
    CHECK(frames <= 4800 + 64);

    // Doppelte Tonhöhe: halb so lang.
    const audio::Voice fast =
        engine.play(sound, audio::PlayParams{.mix = {.left = 1.0f, .right = 1.0f, .pitch = 2.0f}});
    const u64 short_frames = render_until_finished(engine, fast, 64, 48000);
    CHECK(short_frames >= 2400);
    CHECK(short_frames <= 2400 + 64);
}

TEST_CASE("Mixer follows gains smoothly and stops with a fade", "[audio]") {
    audio::AudioEngine engine{manual()};
    const auto sound = memory_sound(sine(48000, 2.0));
    const audio::Voice voice =
        engine.play(sound, audio::PlayParams{.loop = true, .mix = {.left = 1.0f, .right = 0.0f}});
    auto* device = engine.manual_device();
    const Levels only_left = rms(device->render(2048));
    CHECK(only_left.left > 0.3f);
    CHECK(only_left.right == 0.0f);

    engine.set(voice, audio::VoiceMix{.left = 0.0f, .right = 1.0f});
    // Der erste Block blendet über, ohne Sprung: Der erste Frame ist noch fast wie vorher.
    const auto ramp = device->render(512);
    CHECK(std::fabs(ramp[1]) < 0.01f);
    const Levels only_right = rms(device->render(2048));
    CHECK(only_right.left == 0.0f);
    CHECK(only_right.right > 0.3f);

    engine.stop(voice, 0.02f);
    const u64 frames = render_until_finished(engine, voice, 128, 48000);
    // 20 ms Ausblenden: knapp 1000 Frames.
    CHECK(frames >= 900);
    CHECK(frames <= 1200);
}

TEST_CASE("Loops play until stopped", "[audio]") {
    audio::AudioEngine engine{manual()};
    const auto sound = memory_sound(sine(48000, 0.01));
    const audio::Voice voice = engine.play(sound, audio::PlayParams{.loop = true});
    for (i32 i = 0; i < 50; ++i) {
        engine.manual_device()->render(512);
        engine.update();
    }
    CHECK(engine.playing(voice));
    engine.stop(voice);
    render_until_finished(engine, voice, 512, 48000);
    CHECK_FALSE(engine.playing(voice));
}

TEST_CASE("Buses scale and mute their voices", "[audio]") {
    audio::AudioEngine engine{manual()};
    const auto sound = memory_sound(sine(48000, 2.0));
    engine.play(sound, audio::PlayParams{.bus = audio::AudioBus::Music, .loop = true});
    auto* device = engine.manual_device();
    device->render(1024);
    const f32 full = rms(device->render(4096)).left;

    engine.set_bus_volume(audio::AudioBus::Music, 0.5f);
    device->render(1024);
    CHECK_THAT(rms(device->render(4096)).left, WithinAbs(full * 0.5f, 0.01));

    engine.set_bus_muted(audio::AudioBus::Music, true);
    device->render(1024);
    CHECK(rms(device->render(4096)).left == 0.0f);

    engine.set_bus_muted(audio::AudioBus::Music, false);
    engine.set_bus_volume(audio::AudioBus::Master, 0.25f);
    device->render(1024);
    CHECK_THAT(rms(device->render(4096)).left, WithinAbs(full * 0.125f, 0.01));
    // Andere Busse sind nicht betroffen.
    CHECK(engine.bus_volume(audio::AudioBus::Effects) == 1.0f);
}

TEST_CASE("Quiet voices beyond the audible limit run virtually", "[audio]") {
    audio::AudioEngine engine{manual()};
    const auto sound = memory_sound(sine(48000, 1.0, 440.0f, 1, 0.01f));
    for (u32 i = 0; i < 100; ++i) {
        const f32 gain = 0.1f + 0.01f * static_cast<f32>(i);
        CHECK(engine.play(sound, audio::PlayParams{.mix = {.left = gain, .right = gain}}).valid());
    }
    engine.manual_device()->render(512);
    const auto stats = engine.stats();
    CHECK(stats.mixer.audible == audio::max_audible_voices);
    CHECK(stats.mixer.virtual_voices == 100 - audio::max_audible_voices);
    CHECK(stats.voices == 100);
    // Alle laufen gleich lang, auch die virtuellen.
    for (i32 i = 0; i < 100 && engine.stats().voices > 0; ++i) {
        engine.manual_device()->render(4096);
        engine.update();
    }
    CHECK(engine.stats().voices == 0);
}

TEST_CASE("All voice slots can be used and freed", "[audio]") {
    audio::AudioEngine engine{manual()};
    const auto sound = memory_sound(sine(48000, 0.001));
    std::vector<audio::Voice> voices;
    for (u32 i = 0; i < audio::max_voices; ++i) {
        voices.push_back(engine.play(sound, {}));
        CHECK(voices.back().valid());
    }
    CHECK_FALSE(engine.play(sound, {}).valid());
    engine.manual_device()->render(512);
    engine.update();
    CHECK(engine.stats().voices == 0);
    // Neue Kennung auf altem Platz: die alte gilt nicht mehr.
    const audio::Voice again = engine.play(sound, {});
    CHECK(again.valid());
    CHECK(std::ranges::none_of(voices, [&](const audio::Voice& v) { return v == again; }));
}

TEST_CASE("Sound sets parse and write", "[audio]") {
    const audio::SoundSet set{
        .sounds = {11, 22, 33}, .volume_variation = 0.25f, .pitch_variation = 0.125f};
    const auto parsed = audio::parse_sound_set(audio::write_sound_set(set));
    REQUIRE(parsed);
    CHECK(*parsed == set);
    CHECK_FALSE(audio::parse_sound_set(R"({"format":"falsch","sounds":[]})"));
    CHECK_FALSE(audio::parse_sound_set(R"({"format":"ngine.soundset","sounds":[0]})"));
}

TEST_CASE("Sound files decode in memory or as a stream", "[audio]") {
    if (!audio::audio_backend_available()) {
        SKIP("ohne Audio gebaut");
    }
    const auto dir = temp_dir();
    const auto short_path = dir / "kurz.wav";
    const auto long_path = dir / "lang.wav";
    write_wav(short_path, sine(22050, 0.5, 440.0f, 2));
    write_wav(long_path, sine(8000, audio::stream_threshold_seconds + 1.0));

    const auto short_sound = audio::load_sound_file(short_path.string());
    REQUIRE(short_sound);
    CHECK_FALSE(short_sound->streamed());
    CHECK(short_sound->data->channels == 2);
    CHECK(short_sound->data->sample_rate == 22050);
    CHECK(short_sound->data->frames() == 11025);
    CHECK_THAT(short_sound->data->samples[200], WithinAbs(short_sound->data->samples[201], 1e-4));

    const auto long_sound = audio::load_sound_file(long_path.string());
    REQUIRE(long_sound);
    CHECK(long_sound->streamed());
    CHECK(long_sound->info.sample_rate == 8000);
    CHECK(long_sound->info.frames == 8000 * 6);

    CHECK_FALSE(audio::load_sound_file((dir / "fehlt.wav").string()));
    std::ofstream{dir / "kaputt.wav"} << "kein Klang";
    CHECK_FALSE(audio::load_sound_file((dir / "kaputt.wav").string()));
}

TEST_CASE("Streams play without losing frames", "[audio]") {
    if (!audio::audio_backend_available()) {
        SKIP("ohne Audio gebaut");
    }
    const auto path = temp_dir() / "strom.wav";
    write_wav(path, sine(48000, 1.0));
    audio::Sound sound;
    sound.stream_path = path.string();
    sound.info = audio::SoundInfo{.channels = 1, .sample_rate = 48000, .frames = 48000};

    audio::AudioEngine engine{manual()};
    const audio::Voice voice = engine.play(sound, {});
    REQUIRE(voice.valid());
    u64 loud = 0;
    for (i32 i = 0; i < 4000 && engine.playing(voice); ++i) {
        const auto block = engine.manual_device()->render(480);
        for (usize f = 0; f < block.size() / 2; ++f) {
            if (block[f * 2] != 0.0f) {
                ++loud;
            }
        }
        engine.update();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    CHECK_FALSE(engine.playing(voice));
    // Jeder Frame kommt genau einmal (Nulldurchgänge des Sinus zählen nicht als laut).
    CHECK(loud > 47000);
    CHECK(loud <= 48000);
    CHECK(engine.stats().streams.streams == 0);
}

TEST_CASE("Audio system places sources relative to the listener", "[audio]") {
    reflect::TypeRegistry types;
    world::register_types(types);
    audio::register_types(types);
    world::World world{types};

    asset::ResourceManager resources;
    audio::register_loaders(resources);
    const u64 id = asset::generated_id("test-ton");
    resources.add<audio::Sound>(id, memory_sound(sine(48000, 1.0)));

    audio::AudioEngine engine{manual()};
    audio::AudioSystem system{engine, resources};

    world::Entity listener = world.create("Hörer");
    listener.set(world::Transform{}).set(audio::AudioListener{});
    world::Entity source = world.create("Quelle");
    source.set(world::Transform{.position = {4.0, 0.0, 0.0}})
        .set(audio::AudioSource{.sound = id, .loop = true, .max_distance = 100.0f});

    world::update_transforms(world);
    system.update(world, 1.0 / 60.0);
    CHECK(system.playing(source.id()));
    auto* device = engine.manual_device();
    device->render(1024);
    const Levels near = rms(device->render(4096));
    CHECK(near.right > near.left * 10.0f);

    // Weiter weg: leiser (inverse Abschwächung, 4 m zu 16 m).
    source.set(world::Transform{.position = {16.0, 0.0, 0.0}});
    world::update_transforms(world);
    system.update(world, 1.0 / 60.0);
    device->render(1024);
    const Levels far = rms(device->render(4096));
    CHECK_THAT(far.right / near.right, WithinAbs(0.25, 0.02));

    // Ohne Ortung gleich laut auf beiden Seiten.
    source.get_mut<audio::AudioSource>()->spatial = false;
    world::update_transforms(world);
    system.update(world, 1.0 / 60.0);
    device->render(1024);
    const Levels flat = rms(device->render(4096));
    CHECK_THAT(flat.left, WithinAbs(flat.right, 1e-5));

    // Entity weg: Stimme endet.
    source.destroy();
    system.update(world, 1.0 / 60.0);
    for (i32 i = 0; i < 4; ++i) {
        device->render(1024);
    }
    system.update(world, 1.0 / 60.0);
    CHECK(engine.stats().voices == 0);
    CHECK(system.stats().sources == 0);
}

TEST_CASE("Audio system plays one-shots and sound sets", "[audio]") {
    reflect::TypeRegistry types;
    world::register_types(types);
    audio::register_types(types);
    world::World world{types};
    asset::ResourceManager resources;
    audio::register_loaders(resources);
    const u64 a = asset::generated_id("a");
    resources.add<audio::Sound>(a, memory_sound(sine(48000, 0.05)));

    audio::AudioEngine engine{manual()};
    audio::AudioSystem system{engine, resources};
    system.set_fallback_listener(math::Affine{});
    system.update(world, 1.0 / 60.0);

    CHECK(system.play_at(a, math::DVec3{-3.0, 0.0, 0.0}));
    CHECK(system.stats().one_shots == 1);
    auto* device = engine.manual_device();
    const Levels level = rms(device->render(1024));
    CHECK(level.left > level.right * 10.0f);
    for (i32 i = 0; i < 4; ++i) {
        device->render(1024);
    }
    system.update(world, 1.0 / 60.0);
    CHECK(system.stats().one_shots == 0);
    CHECK_FALSE(system.play_at(0, std::nullopt));
}

TEST_CASE("Realtime null device mixes in its own thread", "[audio]") {
    audio::AudioEngine engine{audio::AudioOptions{.mode = audio::DeviceMode::Null}};
    CHECK(engine.device().kind() == audio::DeviceKind::Null);
    CHECK(engine.manual_device() == nullptr);
    const auto sound = memory_sound(sine(48000, 0.02));
    std::vector<audio::Voice> voices;
    for (i32 frame = 0; frame < 60; ++frame) {
        voices.push_back(engine.play(sound, audio::PlayParams{.bus = audio::AudioBus::Effects}));
        engine.set_bus_volume(audio::AudioBus::Effects, 0.5f + 0.005f * static_cast<f32>(frame));
        for (const audio::Voice& v : voices) {
            engine.set(v, audio::VoiceMix{.left = 0.5f, .right = 0.5f});
        }
        engine.update();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (engine.stats().voices > 0 && std::chrono::steady_clock::now() < deadline) {
        engine.update();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto stats = engine.stats();
    CHECK(stats.voices == 0);
    CHECK(stats.mixer.blocks > 0);
    CHECK(stats.mixer.dropped_commands == 0);
}
