module;

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module ngine.audio;

import ngine.core;

namespace ngine::audio {

AudioEngine::AudioEngine(const AudioOptions& options)
    : mixer_{std::make_unique<Mixer>(options.device.sample_rate)},
      streamer_{std::make_unique<Streamer>()} {
    if (options.mode == DeviceMode::Automatic) {
        auto device = AudioDevice::open_system(*mixer_, options.device);
        if (device) {
            device_ = std::move(*device);
            log::info("Audio: {} mit {} Hz", device_->name(), device_->sample_rate());
        } else {
            log::warn("Audio: kein Gerät ({}), weiter mit dem Null-Gerät", device.error());
        }
    }
    if (!device_) {
        device_ = std::make_unique<NullDevice>(*mixer_, options.mode != DeviceMode::Manual,
                                               options.device);
    }
    free_.reserve(max_voices);
    for (u32 slot = max_voices; slot > 0; --slot) {
        free_.push_back(slot - 1);
    }
    volume_.fill(1.0f);
    muted_.fill(false);
}

AudioEngine::~AudioEngine() {
    // Erst das Gerät anhalten, dann den Strom-Thread: Danach liest niemand mehr die Daten der
    // Stimmen.
    device_.reset();
    streamer_.reset();
}

NullDevice* AudioEngine::manual_device() noexcept {
    auto* null = dynamic_cast<NullDevice*>(device_.get());
    return null != nullptr && !null->realtime() ? null : nullptr;
}

u32 AudioEngine::sample_rate() const noexcept {
    return mixer_->sample_rate();
}

AudioEngine::Slot* AudioEngine::find(Voice voice) noexcept {
    if (voice.slot >= max_voices) {
        return nullptr;
    }
    Slot& slot = slots_[voice.slot];
    return slot.used && slot.generation == voice.generation ? &slot : nullptr;
}

const AudioEngine::Slot* AudioEngine::find(Voice voice) const noexcept {
    if (voice.slot >= max_voices) {
        return nullptr;
    }
    const Slot& slot = slots_[voice.slot];
    return slot.used && slot.generation == voice.generation ? &slot : nullptr;
}

Voice AudioEngine::play(const Sound& sound, const PlayParams& params) {
    if (free_.empty()) {
        return {};
    }
    const u32 index = free_.back();
    Slot& slot = slots_[index];

    MixerCommand command{.type = CommandType::Start,
                         .slot = index,
                         .mix = params.mix,
                         .bus = params.bus,
                         .loop = params.loop,
                         .spatial = params.spatial};
    std::shared_ptr<StreamBuffer> stream;
    if (sound.streamed()) {
        if (sound.stream_path.empty() || sound.info.channels == 0) {
            return {};
        }
        stream = std::make_shared<StreamBuffer>(sound.stream_path, sound.info.channels,
                                                sound.info.sample_rate, params.loop,
                                                mixer_->sample_rate());
        command.source = VoiceSource{.stream = stream.get(),
                                     .channels = stream->channels,
                                     .sample_rate = stream->sample_rate};
    } else {
        command.source = VoiceSource{.samples = sound.data->samples.data(),
                                     .frames = sound.data->frames(),
                                     .channels = sound.data->channels,
                                     .sample_rate = sound.data->sample_rate};
    }
    if (!mixer_->send(command)) {
        return {};
    }
    free_.pop_back();
    slot.used = true;
    slot.generation = slot.generation + 1 == 0 ? 1 : slot.generation + 1;
    slot.data = sound.data;
    slot.stream = stream;
    ++used_;
    if (stream) {
        streamer_->add(std::move(stream));
    }
    return Voice{.slot = index, .generation = slot.generation};
}

void AudioEngine::set(Voice voice, const VoiceMix& mix) {
    if (find(voice) != nullptr) {
        mixer_->send(MixerCommand{.type = CommandType::Update, .slot = voice.slot, .mix = mix});
    }
}

void AudioEngine::stop(Voice voice, f32 fade) {
    if (find(voice) != nullptr) {
        mixer_->send(MixerCommand{.type = CommandType::Stop, .slot = voice.slot, .fade = fade});
    }
}

void AudioEngine::stop_all(f32 fade) {
    for (u32 index = 0; index < max_voices; ++index) {
        if (slots_[index].used) {
            mixer_->send(MixerCommand{.type = CommandType::Stop, .slot = index, .fade = fade});
        }
    }
}

bool AudioEngine::playing(Voice voice) const noexcept {
    return find(voice) != nullptr;
}

void AudioEngine::send_bus(AudioBus bus) {
    const u32 index = bus_index(bus) % bus_count;
    mixer_->send(MixerCommand{
        .type = CommandType::BusGain, .bus = bus, .gain = muted_[index] ? 0.0f : volume_[index]});
}

void AudioEngine::set_bus_volume(AudioBus bus, f32 volume) {
    volume_[bus_index(bus) % bus_count] = std::clamp(volume, 0.0f, 4.0f);
    send_bus(bus);
}

f32 AudioEngine::bus_volume(AudioBus bus) const noexcept {
    return volume_[bus_index(bus) % bus_count];
}

void AudioEngine::set_bus_muted(AudioBus bus, bool muted) {
    muted_[bus_index(bus) % bus_count] = muted;
    send_bus(bus);
}

bool AudioEngine::bus_muted(AudioBus bus) const noexcept {
    return muted_[bus_index(bus) % bus_count];
}

void AudioEngine::release(u32 index) {
    Slot& slot = slots_[index];
    if (!slot.used) {
        return;
    }
    if (slot.stream) {
        slot.stream->released.store(true, std::memory_order_release);
        streamer_->remove(slot.stream.get());
    }
    slot.used = false;
    slot.data.reset();
    slot.stream.reset();
    free_.push_back(index);
    --used_;
}

void AudioEngine::update() {
    MixerEvent event;
    while (mixer_->poll(event)) {
        if (event.type == EventType::Finished && event.slot < max_voices) {
            release(event.slot);
        }
    }
}

AudioStats AudioEngine::stats() {
    return AudioStats{.mixer = mixer_->stats(), .streams = streamer_->stats(), .voices = used_};
}

} // namespace ngine::audio
