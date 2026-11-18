module;

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module ngine.audio:sound;

import ngine.core;
import ngine.asset;
import :types;
import :decoder;

// Klänge als Assets (AUD-01): .wav, .ogg, .flac und .mp3 mit .meta wie jedes Asset (ADR 0033).
// Kurze Klänge liegen dekodiert im Speicher, lange öffnet die Engine beim Abspielen als Strom.
// Klangsätze (.soundset) wählen zufällig einen von mehreren Klängen und streuen Lautstärke und
// Tonhöhe.
export namespace ngine::audio {

// Ressource eines Klangs.
struct Sound {
    // Im Speicher, wenn kürzer als stream_threshold_seconds.
    std::shared_ptr<const SoundData> data;
    // Sonst die Datei für den Strom.
    std::string stream_path;
    SoundInfo info;

    [[nodiscard]] bool streamed() const noexcept {
        return data == nullptr;
    }
};

inline constexpr std::string_view sound_set_format = "ngine.soundset";
inline constexpr i64 sound_set_version = 1;

// Ressource eines Klangsatzes.
//
//   { "format": "ngine.soundset", "version": 1, "sounds": [ <Asset-Kennung>, ... ],
//     "volume_variation": 0.1, "pitch_variation": 0.05 }
struct SoundSet {
    std::vector<u64> sounds;
    // Streuung um 1: Lautstärke zwischen 1 − v und 1, Tonhöhe zwischen 1 − p und 1 + p.
    f32 volume_variation = 0.0f;
    f32 pitch_variation = 0.0f;

    friend bool operator==(const SoundSet&, const SoundSet&) = default;
};

// Liest eine Klangdatei: kurz ganz dekodiert, lang nur die Kopfdaten für den Strom.
[[nodiscard]] Result<Sound> load_sound_file(std::string_view path);

[[nodiscard]] Result<SoundSet> parse_sound_set(std::string_view text);
[[nodiscard]] std::string write_sound_set(const SoundSet& set);

// Lader für Sound und SoundSet im Ressourcen-Manager (Laden im Hintergrund, Hot Reload).
void register_loaders(asset::ResourceManager& resources);

} // namespace ngine::audio
