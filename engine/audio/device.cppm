module;

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

export module ngine.audio:device;

import ngine.core;
import :types;
import :mixer;

// Gerät (AUD-01): die schmale Schnittstelle zum Betriebssystem. Ein Gerät ruft den Mixer in
// seinem eigenen Thread auf und gibt das Ergebnis aus. Zwei Umsetzungen: miniaudio
// (device_miniaudio.cpp; Linux über PipeWire, PulseAudio oder ALSA, Windows über WASAPI) und das
// Null-Gerät für --headless, den Server, die CI und Tests.
export namespace ngine::audio {

enum class DeviceKind : u8 {
    System,
    Null,
};

struct DeviceOptions {
    u32 sample_rate = 48000;
    // Länge eines Blocks in Millisekunden.
    u32 period_ms = 10;
};

class AudioDevice {
public:
    AudioDevice() = default;
    virtual ~AudioDevice() = default;
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Öffnet das Standardgerät des Systems und startet es. Der Mixer muss das Gerät überleben.
    [[nodiscard]] static Result<std::unique_ptr<AudioDevice>>
    open_system(Mixer& mixer, const DeviceOptions& options);

    [[nodiscard]] virtual DeviceKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual u32 sample_rate() const noexcept = 0;
};

// Null-Gerät: mischt ohne Ausgabe. Mit realtime ruft es den Mixer in einem eigenen Thread im Takt
// der Echtzeit auf (wie ein echtes Gerät, für --headless und ThreadSanitizer); ohne mischt es nur
// auf Anfrage mit render() (Tests).
class NullDevice final : public AudioDevice {
public:
    NullDevice(Mixer& mixer, bool realtime, const DeviceOptions& options = {});
    ~NullDevice() override;

    [[nodiscard]] DeviceKind kind() const noexcept override {
        return DeviceKind::Null;
    }
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] u32 sample_rate() const noexcept override;
    [[nodiscard]] bool realtime() const noexcept;

    // Nur ohne realtime: mischt frames Frames und liefert sie (Stereo, gültig bis zum nächsten
    // Aufruf).
    std::span<const f32> render(u32 frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::audio
