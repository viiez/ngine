module;

#include <expected>
#include <string>
#include <string_view>
#include <vector>

export module ngine.audio:types;

import ngine.core;

export namespace ngine::audio {

template <typename T>
using Result = std::expected<T, std::string>;

// Busse für Lautstärken (AUD-01). Jede Stimme hängt an genau einem Bus; alle Busse außer Master
// laufen in den Master. Die Reihenfolge ist Teil der Szenen (Enum in AudioSource).
enum class AudioBus : u8 {
    Master,
    Music,
    Effects,
    Ambient,
    Interface,
};
inline constexpr u32 bus_count = 5;

[[nodiscard]] constexpr u32 bus_index(AudioBus bus) noexcept {
    return static_cast<u32>(bus);
}
[[nodiscard]] std::string_view to_string(AudioBus bus) noexcept;

// Kurve der Abschwächung mit dem Abstand (AUD-02), zwischen min_distance und max_distance.
enum class Rolloff : u8 {
    // min / (min + faktor · (d − min)): natürlich, wird nie ganz still.
    Inverse,
    // Gerade von 1 bei min auf 1 − faktor bei max (mit Faktor 1 still bei max).
    Linear,
    // (d / min)^(−faktor).
    Exponential,
};

// Dekodierter Klang im Speicher: Samples als f32, bei Stereo abwechselnd links und rechts.
struct SoundData {
    std::vector<f32> samples;
    // 1 (Mono) oder 2 (Stereo); mehr Kanäle mischt der Decoder auf Stereo.
    u32 channels = 1;
    u32 sample_rate = 48000;

    [[nodiscard]] u64 frames() const noexcept {
        return channels == 0 ? 0 : samples.size() / channels;
    }
    [[nodiscard]] f64 duration() const noexcept {
        return sample_rate == 0 ? 0.0 : static_cast<f64>(frames()) / sample_rate;
    }
};

// Kennung einer Stimme im Mixer: Platz und Generation, damit eine alte Kennung nach dem Ende der
// Stimme nicht eine neue auf demselben Platz trifft.
struct Voice {
    u32 slot = 0;
    u32 generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation != 0;
    }
    friend constexpr bool operator==(const Voice&, const Voice&) = default;
};

// Grenzen des Mixers.
inline constexpr u32 max_voices = 256;
// So viele Stimmen mischt der Mixer höchstens; leisere laufen virtuell weiter (nur ihre Lage).
inline constexpr u32 max_audible_voices = 64;
// Größter Block, den der Mixer am Stück rechnet; größere Anfragen teilt er auf.
inline constexpr u32 max_block_frames = 4096;
// Ausgabe: immer Stereo.
inline constexpr u32 output_channels = 2;
// Grenzen der Tonhöhe (Pitch mal Doppler).
inline constexpr f32 min_pitch = 0.125f;
inline constexpr f32 max_pitch = 4.0f;

} // namespace ngine::audio
