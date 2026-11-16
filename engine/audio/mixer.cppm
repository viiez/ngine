module;

#include <atomic>
#include <memory>
#include <span>

export module ngine.audio:mixer;

import ngine.core;
import :types;
import :stream;

// Mixer (AUD-01, Abstimmung zu R4.5: selbst gebaut). Läuft im Thread des Geräts: Er nimmt Befehle
// aus einer Warteschlange ohne Sperren, mischt bis zu max_audible_voices Stimmen mit linearem
// Resampling auf die Rate des Geräts, blendet Lautstärken über den Block weich, führt die Busse
// zusammen und begrenzt am Ende weich. Im Rückruf wird nie gesperrt, nie Speicher angefordert und
// nie von der Platte gelesen.
export namespace ngine::audio {

// Woraus eine Stimme liest: Samples im Speicher (samples, frames) oder ein Strom. Die Daten
// gehören dem Hauptthread und müssen leben, bis der Mixer die Stimme als beendet meldet.
struct VoiceSource {
    const f32* samples = nullptr;
    u64 frames = 0;
    StreamBuffer* stream = nullptr;
    u32 channels = 1;
    u32 sample_rate = 48000;
};

// Was sich pro Bild an einer Stimme ändert.
struct VoiceMix {
    f32 left = 1.0f;
    f32 right = 1.0f;
    // Tonhöhe samt Doppler; begrenzt auf min_pitch bis max_pitch.
    f32 pitch = 1.0f;
};

enum class CommandType : u8 {
    // Stimme auf slot starten (source, mix, bus, loop, spatial).
    Start,
    // Lautstärken und Tonhöhe ändern (mix).
    Update,
    // Ausblenden über fade Sekunden, dann beenden.
    Stop,
    // Lautstärke eines Busses (bus, gain).
    BusGain,
};

struct MixerCommand {
    CommandType type = CommandType::Update;
    u32 slot = 0;
    VoiceSource source;
    VoiceMix mix;
    AudioBus bus = AudioBus::Effects;
    bool loop = false;
    // Stereo-Quellen werden zu Mono gemischt und dann wie Mono verteilt.
    bool spatial = false;
    f32 fade = 0.0f;
    f32 gain = 1.0f;
};

enum class EventType : u8 {
    // Stimme ist zu Ende (abgespielt oder gestoppt); der Platz ist frei.
    Finished,
};

struct MixerEvent {
    EventType type = EventType::Finished;
    u32 slot = 0;
};

// Zähler für das Fenster „Audio“ und Messungen. Werte aus dem Misch-Thread, ungefähr zeitgleich.
struct MixerStats {
    u64 blocks = 0;
    u64 frames = 0;
    u32 active = 0;
    u32 audible = 0;
    u32 virtual_voices = 0;
    // Leerläufe der Ströme seit dem Start.
    u64 underruns = 0;
    // Befehle, die nicht in die Schlange passten.
    u64 dropped_commands = 0;
    // Rechenzeit des letzten und des längsten Blocks in Millisekunden und Anteil an der Dauer
    // des Blocks (Last, 1 heißt: zu spät).
    f64 last_block_ms = 0.0;
    f64 max_block_ms = 0.0;
    f64 load = 0.0;
    // Höchster Ausschlag der Ausgabe seit dem letzten Abruf von stats() (vor dem Begrenzer).
    f32 peak = 0.0f;
};

class Mixer {
public:
    explicit Mixer(u32 sample_rate);
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    [[nodiscard]] u32 sample_rate() const noexcept;

    // Hauptthread: Befehl an den Misch-Thread. false, wenn die Schlange voll ist.
    bool send(const MixerCommand& command) noexcept;
    // Hauptthread: nächstes Ereignis aus dem Misch-Thread.
    bool poll(MixerEvent& event) noexcept;

    // Misch-Thread: füllt out (Stereo, abwechselnd links und rechts) mit dem nächsten Block.
    void render(std::span<f32> out) noexcept;

    // Jeder Thread.
    [[nodiscard]] MixerStats stats() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::audio
