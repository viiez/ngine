module;

#include <array>
#include <memory>
#include <string_view>
#include <vector>

export module ngine.audio:engine;

import ngine.core;
import :types;
import :mixer;
import :device;
import :stream;
import :sound;

// Audio im Hauptthread (AUD-01): besitzt Mixer, Gerät und Strom-Thread, vergibt die Plätze der
// Stimmen und hält die Daten jeder Stimme, bis der Mixer sie als beendet meldet. Nur im
// Hauptthread benutzen.
export namespace ngine::audio {

enum class DeviceMode : u8 {
    // Gerät des Systems, bei einem Fehler das Null-Gerät im Echtzeit-Takt (mit Warnung).
    Automatic,
    // Null-Gerät im Echtzeit-Takt (--headless, --server, --no-audio).
    Null,
    // Null-Gerät, das nur auf NullDevice::render() mischt (Tests).
    Manual,
};

struct AudioOptions {
    DeviceMode mode = DeviceMode::Automatic;
    DeviceOptions device;
};

// Wie eine Stimme beginnt.
struct PlayParams {
    AudioBus bus = AudioBus::Effects;
    bool loop = false;
    bool spatial = false;
    VoiceMix mix;
};

struct AudioStats {
    MixerStats mixer;
    StreamerStats streams;
    // Stimmen, die der Hauptthread für belegt hält.
    u32 voices = 0;
};

class AudioEngine {
public:
    explicit AudioEngine(const AudioOptions& options = {});
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] const AudioDevice& device() const noexcept {
        return *device_;
    }
    // Das Null-Gerät im Modus Manual, sonst nullptr.
    [[nodiscard]] NullDevice* manual_device() noexcept;
    [[nodiscard]] u32 sample_rate() const noexcept;

    // Startet eine Stimme. Ungültig, wenn alle Plätze belegt sind oder die Schlange voll ist.
    Voice play(const Sound& sound, const PlayParams& params);
    // Lautstärken und Tonhöhe (weich über den nächsten Block).
    void set(Voice voice, const VoiceMix& mix);
    // Ausblenden über fade Sekunden, dann beenden.
    void stop(Voice voice, f32 fade = 0.01f);
    void stop_all(f32 fade = 0.05f);
    // Ob die Stimme noch läuft (bis update() ihr Ende sieht).
    [[nodiscard]] bool playing(Voice voice) const noexcept;

    // Lautstärke eines Busses (0 bis 1) und stumm; Master gilt für alle.
    void set_bus_volume(AudioBus bus, f32 volume);
    [[nodiscard]] f32 bus_volume(AudioBus bus) const noexcept;
    void set_bus_muted(AudioBus bus, bool muted);
    [[nodiscard]] bool bus_muted(AudioBus bus) const noexcept;

    // Einmal pro Bild: nimmt die Meldungen des Mixers entgegen und gibt beendete Stimmen frei.
    void update();

    [[nodiscard]] AudioStats stats();

private:
    struct Slot {
        u32 generation = 0;
        bool used = false;
        std::shared_ptr<const SoundData> data;
        std::shared_ptr<StreamBuffer> stream;
    };

    [[nodiscard]] Slot* find(Voice voice) noexcept;
    [[nodiscard]] const Slot* find(Voice voice) const noexcept;
    void release(u32 slot);
    void send_bus(AudioBus bus);

    std::unique_ptr<Mixer> mixer_;
    std::unique_ptr<Streamer> streamer_;
    std::unique_ptr<AudioDevice> device_;
    std::array<Slot, max_voices> slots_{};
    std::vector<u32> free_;
    std::array<f32, bus_count> volume_{};
    std::array<bool, bus_count> muted_{};
    u32 used_ = 0;
};

} // namespace ngine::audio
