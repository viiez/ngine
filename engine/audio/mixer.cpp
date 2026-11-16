module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

module ngine.audio;

import ngine.core;

namespace ngine::audio {

namespace {

// Ein- und Ausblenden beim Start und bei Stopp ohne eigene Dauer, gegen Knacken.
constexpr f64 start_fade_seconds = 0.004;
constexpr f64 min_stop_fade_seconds = 0.004;
// Begrenzer: darunter unverändert, darüber weich zu 1.
constexpr f32 limiter_threshold = 0.9f;

[[nodiscard]] f32 soft_clip(f32 x) noexcept {
    const f32 a = std::fabs(x);
    if (a <= limiter_threshold) {
        return x;
    }
    const f32 knee = 1.0f - limiter_threshold;
    const f32 y = limiter_threshold + knee * std::tanh((a - limiter_threshold) / knee);
    return std::copysign(y, x);
}

struct VoiceState {
    bool active = false;
    VoiceSource source;
    AudioBus bus = AudioBus::Effects;
    bool loop = false;
    bool spatial = false;
    // Position in Frames der Quelle; bei Strömen relativ zum Anfang von staging.
    f64 cursor = 0.0;
    // Frames der Quelle pro Frame des Geräts ohne Tonhöhe.
    f64 rate = 1.0;
    VoiceMix current;
    VoiceMix target;
    // Hüllkurve für Ein- und Ausblenden (0 bis 1) und ihre Änderung pro Frame.
    f32 envelope = 0.0f;
    f32 envelope_step = 0.0f;
    bool stopping = false;
    // Lautheit für die Auswahl hörbarer Stimmen.
    f32 loudness = 0.0f;
};

} // namespace

struct Mixer::Impl {
    explicit Impl(u32 rate) : sample_rate{rate} {
        for (auto& buffer : buses) {
            buffer.assign(usize{max_block_frames} * output_channels, 0.0f);
        }
        bus_gain.fill(1.0f);
        bus_target.fill(1.0f);
    }

    u32 sample_rate;
    SpscQueue<MixerCommand, 1024> commands;
    SpscQueue<MixerEvent, 1024> events;
    std::array<VoiceState, max_voices> voices{};
    std::array<std::vector<f32>, bus_count> buses;
    std::array<f32, bus_count> bus_gain{};
    std::array<f32, bus_count> bus_target{};
    std::array<u32, max_voices> order{};

    // Zähler (Misch-Thread schreibt, alle lesen).
    std::atomic<u64> blocks{0};
    std::atomic<u64> frames{0};
    std::atomic<u32> active{0};
    std::atomic<u32> audible{0};
    std::atomic<u32> virtual_voices{0};
    std::atomic<u64> underruns{0};
    std::atomic<u64> dropped_commands{0};
    std::atomic<f64> last_block_ms{0.0};
    std::atomic<f64> max_block_ms{0.0};
    std::atomic<f64> load{0.0};
    std::atomic<f32> peak{0.0f};

    void apply(const MixerCommand& command) noexcept;
    void finish(u32 slot) noexcept;
    void render_block(f32* out, u32 count) noexcept;
    // Liefert false, wenn die Stimme in diesem Block zu Ende ging.
    bool mix_memory(VoiceState& voice, f32* bus, u32 count) noexcept;
    bool mix_stream(VoiceState& voice, f32* bus, u32 count) noexcept;
    bool advance_virtual(VoiceState& voice, u32 count) noexcept;
};

void Mixer::Impl::apply(const MixerCommand& command) noexcept {
    if (command.type == CommandType::BusGain) {
        bus_target[bus_index(command.bus) % bus_count] = std::max(command.gain, 0.0f);
        return;
    }
    if (command.slot >= max_voices) {
        return;
    }
    VoiceState& voice = voices[command.slot];
    switch (command.type) {
    case CommandType::Start: {
        voice = VoiceState{};
        voice.active = true;
        voice.source = command.source;
        voice.source.channels = std::clamp(command.source.channels, 1u, 2u);
        voice.bus = command.bus;
        voice.loop = command.loop;
        voice.spatial = command.spatial;
        voice.rate = static_cast<f64>(std::max(command.source.sample_rate, 1u)) / sample_rate;
        voice.current = command.mix;
        voice.target = command.mix;
        voice.envelope = 0.0f;
        voice.envelope_step = static_cast<f32>(1.0 / (start_fade_seconds * sample_rate));
        break;
    }
    case CommandType::Update:
        if (voice.active) {
            voice.target = command.mix;
        }
        break;
    case CommandType::Stop:
        if (voice.active && !voice.stopping) {
            voice.stopping = true;
            const f64 fade = std::max(static_cast<f64>(command.fade), min_stop_fade_seconds);
            voice.envelope_step = -static_cast<f32>(1.0 / (fade * sample_rate));
        }
        break;
    case CommandType::BusGain: break;
    }
}

void Mixer::Impl::finish(u32 slot) noexcept {
    voices[slot].active = false;
    events.push(MixerEvent{.type = EventType::Finished, .slot = slot});
}

namespace {

// Liest Frame index (Stereo als l, r; Mono doppelt) aus Samples im Speicher.
struct Frame {
    f32 l = 0.0f;
    f32 r = 0.0f;
};

[[nodiscard]] Frame read_frame(const f32* samples, u32 channels, u64 index) noexcept {
    if (channels == 1) {
        const f32 s = samples[index];
        return Frame{s, s};
    }
    return Frame{samples[index * 2], samples[index * 2 + 1]};
}

// Hüllkurve eines Frames weiterzählen; liefert false, wenn das Ausblenden fertig ist.
bool step_envelope(VoiceState& voice) noexcept {
    voice.envelope += voice.envelope_step;
    if (voice.envelope >= 1.0f) {
        voice.envelope = 1.0f;
        voice.envelope_step = 0.0f;
    } else if (voice.envelope <= 0.0f && voice.stopping) {
        voice.envelope = 0.0f;
        return false;
    }
    return true;
}

// Mischt einen interpolierten Frame mit den Lautstärken des Frames in bus.
void put(const VoiceState& voice, Frame frame, f32 left, f32 right, f32* bus) noexcept {
    const f32 env = voice.envelope;
    if (voice.spatial) {
        const f32 mono = voice.source.channels == 1 ? frame.l : 0.5f * (frame.l + frame.r);
        bus[0] += mono * left * env;
        bus[1] += mono * right * env;
    } else {
        bus[0] += frame.l * left * env;
        bus[1] += frame.r * right * env;
    }
}

[[nodiscard]] f32 clamp_pitch(f32 pitch) noexcept {
    return std::clamp(pitch, min_pitch, max_pitch);
}

} // namespace

bool Mixer::Impl::mix_memory(VoiceState& voice, f32* bus, u32 count) noexcept {
    const VoiceSource& src = voice.source;
    if (src.samples == nullptr || src.frames == 0) {
        return false;
    }
    const f64 length = static_cast<f64>(src.frames);
    const f32 inv = 1.0f / static_cast<f32>(count);
    const VoiceMix from = voice.current;
    const VoiceMix to = voice.target;
    for (u32 i = 0; i < count; ++i) {
        if (voice.cursor >= length) {
            if (!voice.loop) {
                return false;
            }
            voice.cursor = std::fmod(voice.cursor, length);
        }
        const auto index = static_cast<u64>(voice.cursor);
        const auto frac = static_cast<f32>(voice.cursor - static_cast<f64>(index));
        u64 next = index + 1;
        if (next >= src.frames) {
            next = voice.loop ? 0 : index;
        }
        const Frame a = read_frame(src.samples, src.channels, index);
        const Frame b = read_frame(src.samples, src.channels, next);
        const Frame frame{a.l + (b.l - a.l) * frac, a.r + (b.r - a.r) * frac};
        const f32 t = static_cast<f32>(i + 1) * inv;
        put(voice, frame, from.left + (to.left - from.left) * t,
            from.right + (to.right - from.right) * t, bus + usize{i} * 2);
        if (!step_envelope(voice)) {
            return false;
        }
        const f32 pitch = clamp_pitch(from.pitch + (to.pitch - from.pitch) * t);
        voice.cursor += voice.rate * pitch;
    }
    return true;
}

bool Mixer::Impl::mix_stream(VoiceState& voice, f32* bus, u32 count) noexcept {
    StreamBuffer& stream = *voice.source.stream;
    const u32 channels = voice.source.channels;
    if (!stream.primed.load(std::memory_order_acquire)) {
        // Noch nicht genug vorrätig: warten, ohne als Leerlauf zu zählen.
        return !stream.failed.load(std::memory_order_acquire);
    }

    // Fenster verschieben: was hinter dem Cursor liegt, wird nicht mehr gebraucht.
    const u64 drop = std::min(static_cast<u64>(voice.cursor), stream.staged);
    if (drop > 0) {
        std::copy(stream.staging.begin() + static_cast<isize>(drop * channels),
                  stream.staging.begin() + static_cast<isize>(stream.staged * channels),
                  stream.staging.begin());
        stream.staged -= drop;
        voice.cursor -= static_cast<f64>(drop);
    }
    // Nachlesen, so viel der Block höchstens braucht.
    const u64 capacity = stream.staging.size() / channels;
    const f64 most = voice.rate * static_cast<f64>(max_pitch);
    const u64 need = std::min(capacity, static_cast<u64>(voice.cursor + most * count) + 2);
    if (need > stream.staged) {
        const usize want = static_cast<usize>(need - stream.staged) * channels;
        const usize got = stream.ring.read(std::span<f32>{
            stream.staging.data() + static_cast<usize>(stream.staged) * channels, want});
        stream.staged += got / channels;
    }

    const bool ended = stream.ended.load(std::memory_order_acquire) && stream.ring.available() == 0;
    const f32 inv = 1.0f / static_cast<f32>(count);
    const VoiceMix from = voice.current;
    const VoiceMix to = voice.target;
    for (u32 i = 0; i < count; ++i) {
        const auto index = static_cast<u64>(voice.cursor);
        if (index + 1 >= stream.staged) {
            if (ended) {
                if (index >= stream.staged) {
                    return false;
                }
            } else {
                // Leerlauf: Rest des Blocks still, der Cursor wartet.
                underruns.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        const u64 next = index + 1 < stream.staged ? index + 1 : index;
        const auto frac = static_cast<f32>(voice.cursor - static_cast<f64>(index));
        const Frame a = read_frame(stream.staging.data(), channels, index);
        const Frame b = read_frame(stream.staging.data(), channels, next);
        const Frame frame{a.l + (b.l - a.l) * frac, a.r + (b.r - a.r) * frac};
        const f32 t = static_cast<f32>(i + 1) * inv;
        put(voice, frame, from.left + (to.left - from.left) * t,
            from.right + (to.right - from.right) * t, bus + usize{i} * 2);
        if (!step_envelope(voice)) {
            return false;
        }
        const f32 pitch = clamp_pitch(from.pitch + (to.pitch - from.pitch) * t);
        voice.cursor += voice.rate * pitch;
    }
    return true;
}

bool Mixer::Impl::advance_virtual(VoiceState& voice, u32 count) noexcept {
    // Nur die Lage weiterzählen; ein Ausblenden endet sofort, weil es ohnehin unhörbar ist.
    if (voice.stopping) {
        return false;
    }
    voice.envelope = 1.0f;
    voice.envelope_step = 0.0f;
    const f64 length = static_cast<f64>(voice.source.frames);
    voice.cursor += voice.rate * static_cast<f64>(clamp_pitch(voice.target.pitch)) * count;
    if (voice.cursor >= length) {
        if (!voice.loop || length <= 0.0) {
            return false;
        }
        voice.cursor = std::fmod(voice.cursor, length);
    }
    return true;
}

void Mixer::Impl::render_block(f32* out, u32 count) noexcept {
    const usize samples = usize{count} * output_channels;
    for (auto& buffer : buses) {
        std::fill_n(buffer.begin(), samples, 0.0f);
    }

    // Lautheit jeder Stimme; Ströme sind immer hörbar, weil sie sonst ihren Vorrat nicht leeren.
    u32 used = 0;
    for (u32 slot = 0; slot < max_voices; ++slot) {
        VoiceState& voice = voices[slot];
        if (!voice.active) {
            continue;
        }
        const u32 bus = bus_index(voice.bus) % bus_count;
        voice.loudness = std::max(voice.target.left, voice.target.right) * bus_target[bus] *
                         (bus == 0 ? 1.0f : bus_target[0]);
        if (voice.source.stream != nullptr) {
            voice.loudness += 1000.0f;
        }
        order[used++] = slot;
    }
    const u32 audible_count = std::min(used, max_audible_voices);
    if (used > max_audible_voices) {
        std::partial_sort(order.begin(), order.begin() + audible_count, order.begin() + used,
                          [this](u32 a, u32 b) {
                              return voices[a].loudness > voices[b].loudness ||
                                     (voices[a].loudness == voices[b].loudness && a < b);
                          });
    }

    for (u32 n = 0; n < used; ++n) {
        const u32 slot = order[n];
        VoiceState& voice = voices[slot];
        bool alive = true;
        if (n < audible_count) {
            f32* bus = buses[bus_index(voice.bus) % bus_count].data();
            alive = voice.source.stream != nullptr ? mix_stream(voice, bus, count)
                                                   : mix_memory(voice, bus, count);
        } else {
            alive = advance_virtual(voice, count);
        }
        voice.current = voice.target;
        if (!alive) {
            finish(slot);
        }
    }

    // Busse in den Master, mit weichem Übergang ihrer Lautstärken.
    const f32 inv = 1.0f / static_cast<f32>(count);
    f32* master = buses[0].data();
    for (u32 b = 1; b < bus_count; ++b) {
        const f32 from = bus_gain[b];
        const f32 to = bus_target[b];
        const f32* source = buses[b].data();
        for (u32 i = 0; i < count; ++i) {
            const f32 gain = from + (to - from) * static_cast<f32>(i + 1) * inv;
            master[i * 2] += source[i * 2] * gain;
            master[i * 2 + 1] += source[i * 2 + 1] * gain;
        }
        bus_gain[b] = to;
    }
    const f32 from = bus_gain[0];
    const f32 to = bus_target[0];
    f32 block_peak = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        const f32 gain = from + (to - from) * static_cast<f32>(i + 1) * inv;
        for (u32 c = 0; c < output_channels; ++c) {
            const f32 value = master[i * 2 + c] * gain;
            block_peak = std::max(block_peak, std::fabs(value));
            out[i * 2 + c] = soft_clip(value);
        }
    }
    bus_gain[0] = to;

    active.store(used, std::memory_order_relaxed);
    audible.store(audible_count, std::memory_order_relaxed);
    virtual_voices.store(used - audible_count, std::memory_order_relaxed);
    if (block_peak > peak.load(std::memory_order_relaxed)) {
        peak.store(block_peak, std::memory_order_relaxed);
    }
}

Mixer::Mixer(u32 sample_rate) : impl_{std::make_unique<Impl>(std::max(sample_rate, 1u))} {}

Mixer::~Mixer() = default;

u32 Mixer::sample_rate() const noexcept {
    return impl_->sample_rate;
}

bool Mixer::send(const MixerCommand& command) noexcept {
    if (!impl_->commands.push(command)) {
        impl_->dropped_commands.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool Mixer::poll(MixerEvent& event) noexcept {
    return impl_->events.pop(event);
}

void Mixer::render(std::span<f32> out) noexcept {
    const auto start = std::chrono::steady_clock::now();
    MixerCommand command;
    while (impl_->commands.pop(command)) {
        impl_->apply(command);
    }
    const auto total = static_cast<u32>(out.size() / output_channels);
    u32 done = 0;
    while (done < total) {
        const u32 count = std::min(total - done, max_block_frames);
        impl_->render_block(out.data() + usize{done} * output_channels, count);
        done += count;
    }
    const f64 ms =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    impl_->blocks.fetch_add(1, std::memory_order_relaxed);
    impl_->frames.fetch_add(total, std::memory_order_relaxed);
    impl_->last_block_ms.store(ms, std::memory_order_relaxed);
    if (ms > impl_->max_block_ms.load(std::memory_order_relaxed)) {
        impl_->max_block_ms.store(ms, std::memory_order_relaxed);
    }
    if (total > 0) {
        const f64 duration_ms = 1000.0 * total / impl_->sample_rate;
        impl_->load.store(ms / duration_ms, std::memory_order_relaxed);
    }
}

MixerStats Mixer::stats() noexcept {
    return MixerStats{
        .blocks = impl_->blocks.load(std::memory_order_relaxed),
        .frames = impl_->frames.load(std::memory_order_relaxed),
        .active = impl_->active.load(std::memory_order_relaxed),
        .audible = impl_->audible.load(std::memory_order_relaxed),
        .virtual_voices = impl_->virtual_voices.load(std::memory_order_relaxed),
        .underruns = impl_->underruns.load(std::memory_order_relaxed),
        .dropped_commands = impl_->dropped_commands.load(std::memory_order_relaxed),
        .last_block_ms = impl_->last_block_ms.load(std::memory_order_relaxed),
        .max_block_ms = impl_->max_block_ms.load(std::memory_order_relaxed),
        .load = impl_->load.load(std::memory_order_relaxed),
        .peak = impl_->peak.exchange(0.0f, std::memory_order_relaxed),
    };
}

} // namespace ngine::audio
