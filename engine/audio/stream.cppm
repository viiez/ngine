module;

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module ngine.audio:stream;

import ngine.core;
import :types;
import :queue;
import :decoder;

// Ströme für lange Klänge (Musik, Atmosphäre): Ein eigener Thread dekodiert voraus in einen
// Ringpuffer pro Stimme, der Mixer liest nur daraus und wartet nie auf die Platte.
export namespace ngine::audio {

// Ab dieser Länge in Sekunden wird ein Klang gestreamt statt ganz dekodiert (Abstimmung zu R4.5).
inline constexpr f64 stream_threshold_seconds = 5.0;
// Vorrat eines Stroms in Sekunden.
inline constexpr f64 stream_buffer_seconds = 1.0;

// Puffer eines Stroms. Der Hauptthread legt ihn an und hält ihn, bis der Mixer die Stimme als
// beendet meldet; der Strom-Thread schreibt in ring, der Mixer liest daraus.
struct StreamBuffer {
    StreamBuffer(std::string file, u32 channels, u32 sample_rate, bool loop, u32 device_rate);

    const std::string path;
    const u32 channels;
    const u32 sample_rate;
    const bool loop;
    SampleRing ring;

    // Strom-Thread: genug vorrätig, der Mixer darf beginnen.
    std::atomic<bool> primed{false};
    // Strom-Thread: Datei zu Ende (ohne Schleife) oder gescheitert; mehr kommt nicht.
    std::atomic<bool> ended{false};
    std::atomic<bool> failed{false};
    // Hauptthread: Stimme ist vorbei, der Strom-Thread hört auf.
    std::atomic<bool> released{false};

    // Nur der Mixer: Fenster der zuletzt gelesenen Frames für das Resampling.
    std::vector<f32> staging;
    u64 staged = 0;

    // Nur der Strom-Thread.
    std::unique_ptr<StreamDecoder> decoder;
};

struct StreamerStats {
    usize streams = 0;
    // Gescheiterte Öffnungen seit dem Start.
    u64 failures = 0;
};

// Thread, der alle Ströme nachfüllt.
class Streamer {
public:
    Streamer();
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    void add(std::shared_ptr<StreamBuffer> stream);
    // Hört auf, den Strom zu füllen; danach darf der Hauptthread ihn freigeben.
    void remove(const StreamBuffer* stream);
    [[nodiscard]] StreamerStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::audio
