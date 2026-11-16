module;

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// Ohne NGINE_WITH_AUDIO: kein Gerät des Systems und kein Decoder, nur das Null-Gerät. Komponenten
// und Assets bleiben bekannt, damit Szenen vollständig bleiben (Abstimmung zu R4.5).
module ngine.audio;

import ngine.core;

namespace ngine::audio {

namespace {
constexpr std::string_view disabled = "ohne Audio gebaut (NGINE_WITH_AUDIO=OFF)";
}

bool audio_backend_available() noexcept {
    return false;
}

Result<SoundInfo> probe_sound(std::span<const std::byte> /*bytes*/) {
    return std::unexpected(std::string{disabled});
}

Result<SoundData> decode_sound(std::span<const std::byte> /*bytes*/) {
    return std::unexpected(std::string{disabled});
}

Result<std::unique_ptr<StreamDecoder>> StreamDecoder::open(std::string_view /*path*/) {
    return std::unexpected(std::string{disabled});
}

Result<std::unique_ptr<AudioDevice>> AudioDevice::open_system(Mixer& /*mixer*/,
                                                              const DeviceOptions& /*options*/) {
    return std::unexpected(std::string{disabled});
}

} // namespace ngine::audio
