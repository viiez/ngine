module;

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NULL
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#include <miniaudio.h>

// Adapter für miniaudio (ADR 0049): Gerät über ma_device und Dekodieren über ma_decoder. Nur diese
// Datei (und miniaudio_impl.cpp mit der Umsetzung) kennt die Bibliothek.
module ngine.audio;

import ngine.core;

namespace ngine::audio {

namespace {

[[nodiscard]] std::string describe(ma_result result) {
    return std::format("{} ({})", ma_result_description(result), static_cast<int>(result));
}

// Konfiguration des Decoders: f32 in der Rate der Datei, höchstens zwei Kanäle.
[[nodiscard]] ma_decoder_config decoder_config(u32 channels) {
    return ma_decoder_config_init(ma_format_f32, channels, 0);
}

// Öffnet einen Decoder und mischt mehr als zwei Kanäle auf Stereo herunter.
template <typename Init>
ma_result open_decoder(ma_decoder& decoder, Init&& init) {
    ma_decoder_config config = decoder_config(0);
    ma_result result = init(config, decoder);
    if (result != MA_SUCCESS) {
        return result;
    }
    if (decoder.outputChannels > 2) {
        ma_decoder_uninit(&decoder);
        config = decoder_config(2);
        result = init(config, decoder);
    }
    return result;
}

[[nodiscard]] SoundInfo info_of(ma_decoder& decoder) {
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) != MA_SUCCESS) {
        frames = 0;
    }
    return SoundInfo{.channels = decoder.outputChannels,
                     .sample_rate = decoder.outputSampleRate,
                     .frames = frames};
}

class MiniaudioStream final : public StreamDecoder {
public:
    MiniaudioStream() = default;
    ~MiniaudioStream() override {
        if (open_) {
            ma_decoder_uninit(&decoder_);
        }
    }
    MiniaudioStream(const MiniaudioStream&) = delete;
    MiniaudioStream& operator=(const MiniaudioStream&) = delete;

    Result<void> open(const std::string& path) {
        const ma_result result = open_decoder(decoder_, [&](ma_decoder_config& c, ma_decoder& d) {
            return ma_decoder_init_file(path.c_str(), &c, &d);
        });
        if (result != MA_SUCCESS) {
            return std::unexpected(describe(result));
        }
        open_ = true;
        info_ = info_of(decoder_);
        return {};
    }

    [[nodiscard]] SoundInfo info() const noexcept override {
        return info_;
    }

    u64 read(std::span<f32> out) override {
        if (info_.channels == 0) {
            return 0;
        }
        ma_uint64 read = 0;
        const ma_result result =
            ma_decoder_read_pcm_frames(&decoder_, out.data(), out.size() / info_.channels, &read);
        if (result != MA_SUCCESS && result != MA_AT_END) {
            return 0;
        }
        return read;
    }

    bool rewind() override {
        return ma_decoder_seek_to_pcm_frame(&decoder_, 0) == MA_SUCCESS;
    }

private:
    ma_decoder decoder_{};
    SoundInfo info_;
    bool open_ = false;
};

class MiniaudioDevice final : public AudioDevice {
public:
    MiniaudioDevice() = default;
    ~MiniaudioDevice() override {
        if (initialized_) {
            ma_device_uninit(&device_);
        }
    }
    MiniaudioDevice(const MiniaudioDevice&) = delete;
    MiniaudioDevice& operator=(const MiniaudioDevice&) = delete;

    Result<void> open(Mixer& mixer, const DeviceOptions& options) {
        mixer_ = &mixer;
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = output_channels;
        // Der Mixer rechnet in seiner Rate; miniaudio wandelt, falls das Gerät eine andere hat.
        config.sampleRate = mixer.sample_rate();
        config.periodSizeInMilliseconds = std::max(options.period_ms, 1u);
        config.dataCallback = &MiniaudioDevice::callback;
        config.pUserData = this;
        // Der Mixer füllt jeden Block selbst.
        config.noPreSilencedOutputBuffer = MA_TRUE;
        const ma_result result = ma_device_init(nullptr, &config, &device_);
        if (result != MA_SUCCESS) {
            return std::unexpected(describe(result));
        }
        initialized_ = true;
        const ma_result started = ma_device_start(&device_);
        if (started != MA_SUCCESS) {
            return std::unexpected(describe(started));
        }
        name_ = std::format("{} ({})", device_.playback.name,
                            ma_get_backend_name(device_.pContext->backend));
        return {};
    }

    [[nodiscard]] DeviceKind kind() const noexcept override {
        return DeviceKind::System;
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return name_;
    }
    [[nodiscard]] u32 sample_rate() const noexcept override {
        return mixer_ != nullptr ? mixer_->sample_rate() : 0;
    }

private:
    static void callback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frames) {
        auto* self = static_cast<MiniaudioDevice*>(device->pUserData);
        self->mixer_->render(
            std::span<f32>{static_cast<f32*>(output), usize{frames} * output_channels});
    }

    ma_device device_{};
    Mixer* mixer_ = nullptr;
    std::string name_;
    bool initialized_ = false;
};

} // namespace

bool audio_backend_available() noexcept {
    return true;
}

Result<SoundInfo> probe_sound(std::span<const std::byte> bytes) {
    ma_decoder decoder{};
    const ma_result result = open_decoder(decoder, [&](ma_decoder_config& c, ma_decoder& d) {
        return ma_decoder_init_memory(bytes.data(), bytes.size(), &c, &d);
    });
    if (result != MA_SUCCESS) {
        return std::unexpected(describe(result));
    }
    const SoundInfo info = info_of(decoder);
    ma_decoder_uninit(&decoder);
    return info;
}

Result<SoundData> decode_sound(std::span<const std::byte> bytes) {
    ma_decoder decoder{};
    const ma_result result = open_decoder(decoder, [&](ma_decoder_config& c, ma_decoder& d) {
        return ma_decoder_init_memory(bytes.data(), bytes.size(), &c, &d);
    });
    if (result != MA_SUCCESS) {
        return std::unexpected(describe(result));
    }
    const SoundInfo info = info_of(decoder);
    SoundData data;
    data.channels = info.channels;
    data.sample_rate = info.sample_rate;
    if (info.frames > 0) {
        data.samples.reserve(static_cast<usize>(info.frames) * info.channels);
    }
    // Stückweise, weil nicht jedes Format seine Länge kennt.
    constexpr ma_uint64 chunk = 16384;
    usize filled = 0;
    while (true) {
        data.samples.resize(filled + static_cast<usize>(chunk) * info.channels);
        ma_uint64 read = 0;
        const ma_result r =
            ma_decoder_read_pcm_frames(&decoder, data.samples.data() + filled, chunk, &read);
        filled += static_cast<usize>(read) * info.channels;
        if (r != MA_SUCCESS || read < chunk) {
            break;
        }
    }
    data.samples.resize(filled);
    ma_decoder_uninit(&decoder);
    if (data.samples.empty()) {
        return std::unexpected(std::string{"Die Datei enthält keine Samples"});
    }
    return data;
}

Result<std::unique_ptr<StreamDecoder>> StreamDecoder::open(std::string_view path) {
    auto stream = std::make_unique<MiniaudioStream>();
    if (auto opened = stream->open(std::string{path}); !opened) {
        return std::unexpected(std::move(opened.error()));
    }
    return std::unique_ptr<StreamDecoder>{std::move(stream)};
}

Result<std::unique_ptr<AudioDevice>> AudioDevice::open_system(Mixer& mixer,
                                                              const DeviceOptions& options) {
    auto device = std::make_unique<MiniaudioDevice>();
    if (auto opened = device->open(mixer, options); !opened) {
        return std::unexpected(std::move(opened.error()));
    }
    return std::unique_ptr<AudioDevice>{std::move(device)};
}

} // namespace ngine::audio
