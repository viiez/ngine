module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module ngine.audio;

import ngine.core;

namespace ngine::audio {

namespace {

[[nodiscard]] usize ring_samples(u32 channels, u32 sample_rate) {
    const auto frames = static_cast<usize>(stream_buffer_seconds * sample_rate);
    return std::max<usize>(frames, 4096) * channels;
}

[[nodiscard]] usize staging_samples(u32 channels, u32 sample_rate, u32 device_rate) {
    const f64 ratio = static_cast<f64>(sample_rate) / std::max(device_rate, 1u);
    const auto frames =
        static_cast<usize>(static_cast<f64>(max_block_frames) * ratio * max_pitch) + 8;
    return frames * channels;
}

} // namespace

StreamBuffer::StreamBuffer(std::string file, u32 channel_count, u32 rate, bool looping,
                           u32 device_rate)
    : path{std::move(file)}, channels{std::clamp(channel_count, 1u, 2u)}, sample_rate{rate},
      loop{looping}, ring{ring_samples(channels, rate)},
      staging(staging_samples(channels, rate, device_rate), 0.0f) {}

struct Streamer::Impl {
    Impl() : thread{[this] { run(); }} {}
    ~Impl() {
        {
            const std::lock_guard lock{mutex};
            stop = true;
        }
        wake.notify_all();
        thread.join();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // Füllt einen Strom so weit, wie Platz ist. Liefert true, wenn er etwas geschrieben hat.
    bool fill(StreamBuffer& stream, std::vector<f32>& scratch) {
        if (stream.ended.load(std::memory_order_relaxed) ||
            stream.released.load(std::memory_order_acquire)) {
            return false;
        }
        if (!stream.decoder) {
            auto decoder = StreamDecoder::open(stream.path);
            if (!decoder) {
                log::error("Audio: Strom {} lässt sich nicht öffnen: {}", stream.path,
                           decoder.error());
                failures.fetch_add(1, std::memory_order_relaxed);
                stream.failed.store(true, std::memory_order_release);
                stream.ended.store(true, std::memory_order_release);
                return false;
            }
            stream.decoder = std::move(*decoder);
        }
        bool wrote = false;
        // In Stücken von höchstens einer Zehntelsekunde, nur ganze Frames.
        const usize chunk = std::max<usize>(stream.sample_rate / 10, 256) * stream.channels;
        while (true) {
            const usize room = (stream.ring.free() / stream.channels) * stream.channels;
            if (room < std::min(chunk, stream.ring.capacity() / 2)) {
                break;
            }
            scratch.resize(std::min(room, chunk));
            u64 frames = stream.decoder->read(scratch);
            if (frames == 0 && stream.loop && stream.decoder->rewind()) {
                // Schleife: wieder von vorne.
                frames = stream.decoder->read(scratch);
            }
            if (frames == 0) {
                stream.ended.store(true, std::memory_order_release);
                break;
            }
            stream.ring.write(
                std::span<const f32>{scratch.data(), static_cast<usize>(frames) * stream.channels});
            wrote = true;
        }
        if (!stream.primed.load(std::memory_order_relaxed) &&
            (stream.ring.available() * 4 >= stream.ring.capacity() ||
             stream.ended.load(std::memory_order_relaxed))) {
            stream.primed.store(true, std::memory_order_release);
        }
        return wrote;
    }

    void run() {
        std::vector<f32> scratch;
        std::vector<std::shared_ptr<StreamBuffer>> work;
        std::unique_lock lock{mutex};
        while (!stop) {
            work = streams;
            lock.unlock();
            for (const auto& stream : work) {
                fill(*stream, scratch);
            }
            work.clear();
            lock.lock();
            wake.wait_for(lock, std::chrono::milliseconds{5}, [this] { return stop || dirty; });
            dirty = false;
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::shared_ptr<StreamBuffer>> streams;
    bool stop = false;
    bool dirty = false;
    std::atomic<u64> failures{0};
    std::thread thread;
};

Streamer::Streamer() : impl_{std::make_unique<Impl>()} {}

Streamer::~Streamer() = default;

void Streamer::add(std::shared_ptr<StreamBuffer> stream) {
    {
        const std::lock_guard lock{impl_->mutex};
        impl_->streams.push_back(std::move(stream));
        impl_->dirty = true;
    }
    impl_->wake.notify_all();
}

void Streamer::remove(const StreamBuffer* stream) {
    const std::lock_guard lock{impl_->mutex};
    std::erase_if(impl_->streams, [stream](const auto& s) { return s.get() == stream; });
}

StreamerStats Streamer::stats() const {
    const std::lock_guard lock{impl_->mutex};
    return StreamerStats{.streams = impl_->streams.size(),
                         .failures = impl_->failures.load(std::memory_order_relaxed)};
}

} // namespace ngine::audio
