module;

#include <string_view>

module ngine.audio;

import ngine.core;
import ngine.reflect;

namespace ngine::audio {

std::string_view to_string(AudioBus bus) noexcept {
    switch (bus) {
    case AudioBus::Master: return "Master";
    case AudioBus::Music: return "Musik";
    case AudioBus::Effects: return "Effekte";
    case AudioBus::Ambient: return "Atmosphäre";
    case AudioBus::Interface: return "Oberfläche";
    }
    return "?";
}

void register_types(reflect::TypeRegistry& types) {
    if (types.find<AudioBus>() == nullptr) {
        types.add<AudioBus>("AudioBus")
            .value("Master", AudioBus::Master)
            .value("Music", AudioBus::Music)
            .value("Effects", AudioBus::Effects)
            .value("Ambient", AudioBus::Ambient)
            .value("Interface", AudioBus::Interface);
    }
    if (types.find<Rolloff>() == nullptr) {
        types.add<Rolloff>("Rolloff")
            .value("Inverse", Rolloff::Inverse)
            .value("Linear", Rolloff::Linear)
            .value("Exponential", Rolloff::Exponential);
    }
    if (types.find<AudioSource>() == nullptr) {
        types.add<AudioSource>("AudioSource")
            .field("sound", &AudioSource::sound)
            .hint(reflect::Hint::Asset)
            .field("bus", &AudioSource::bus)
            .field("volume", &AudioSource::volume)
            .range(0.0, 4.0, 0.01)
            .field("pitch", &AudioSource::pitch)
            .range(0.125, 4.0, 0.01)
            .field("loop", &AudioSource::loop)
            .field("play_on_start", &AudioSource::play_on_start)
            .field("spatial", &AudioSource::spatial)
            .field("min_distance", &AudioSource::min_distance)
            .range(0.01, 1000.0, 0.1)
            .field("max_distance", &AudioSource::max_distance)
            .range(0.01, 10000.0, 0.1)
            .field("rolloff", &AudioSource::rolloff)
            .field("rolloff_factor", &AudioSource::rolloff_factor)
            .range(0.0, 10.0, 0.01)
            .field("doppler", &AudioSource::doppler)
            .range(0.0, 4.0, 0.01);
    }
    if (types.find<AudioListener>() == nullptr) {
        types.add<AudioListener>("AudioListener")
            .field("volume", &AudioListener::volume)
            .range(0.0, 4.0, 0.01);
    }
}

} // namespace ngine::audio
