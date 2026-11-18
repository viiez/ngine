module;

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

export module ngine.audio:decoder;

import ngine.core;
import :types;

// Dekodieren von Klangdateien (WAV, Ogg Vorbis, FLAC, MP3). Das Format erkennt der Decoder am
// Inhalt. Hinter dieser Schnittstelle steckt miniaudio mit stb_vorbis (miniaudio.cpp); ohne
// NGINE_WITH_AUDIO meldet sie einen Fehler (miniaudio_null.cpp). Ausgabe immer f32, Mono oder
// Stereo, in der Abtastrate der Datei.
export namespace ngine::audio {

struct SoundInfo {
    u32 channels = 0;
    u32 sample_rate = 0;
    // Länge in Frames; 0, wenn das Format sie nicht verrät.
    u64 frames = 0;

    [[nodiscard]] f64 duration() const noexcept {
        return sample_rate == 0 ? 0.0 : static_cast<f64>(frames) / sample_rate;
    }
};

// Ob Klänge dekodiert und auf einem Gerät ausgegeben werden können (false ohne NGINE_WITH_AUDIO).
[[nodiscard]] bool audio_backend_available() noexcept;

// Kopfdaten einer Datei im Speicher, ohne alles zu dekodieren.
[[nodiscard]] Result<SoundInfo> probe_sound(std::span<const std::byte> bytes);
// Dekodiert die ganze Datei.
[[nodiscard]] Result<SoundData> decode_sound(std::span<const std::byte> bytes);

// Liest eine Datei Stück für Stück (Ströme). Nur von einem Thread zugleich benutzen.
class StreamDecoder {
public:
    virtual ~StreamDecoder() = default;
    StreamDecoder() = default;
    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    [[nodiscard]] static Result<std::unique_ptr<StreamDecoder>> open(std::string_view path);

    [[nodiscard]] virtual SoundInfo info() const noexcept = 0;
    // Liest höchstens out.size() / channels Frames; liefert die Zahl, 0 am Ende.
    virtual u64 read(std::span<f32> out) = 0;
    // Springt an den Anfang (für Schleifen).
    virtual bool rewind() = 0;
};

} // namespace ngine::audio
