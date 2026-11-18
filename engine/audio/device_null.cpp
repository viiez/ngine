module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

module ngine.audio;

import ngine.core;

namespace ngine::audio {

struct NullDevice::Impl {
    Impl(Mixer& m, bool rt, const DeviceOptions& options)
        : mixer{&m}, realtime{rt}, sample_rate{m.sample_rate()},
          period_frames{std::max(sample_rate * std::max(options.period_ms, 1u) / 1000, 16u)} {
        if (realtime) {
            thread = std::thread{[this] { run(); }};
        }
    }
    ~Impl() {
        stop.store(true, std::memory_order_release);
        if (thread.joinable()) {
            thread.join();
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void run() {
        std::vector<f32> buffer(usize{period_frames} * output_channels);
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<f64>(static_cast<f64>(period_frames) / sample_rate));
        auto next = std::chrono::steady_clock::now();
        while (!stop.load(std::memory_order_acquire)) {
            mixer->render(buffer);
            next += period;
            const auto now = std::chrono::steady_clock::now();
            if (next < now - period * 4) {
                // Zu weit zurück (z. B. angehalten im Debugger): nicht aufholen.
                next = now;
            }
            std::this_thread::sleep_until(next);
        }
    }

    Mixer* mixer;
    bool realtime;
    u32 sample_rate;
    u32 period_frames;
    std::vector<f32> output;
    std::atomic<bool> stop{false};
    std::thread thread;
};

NullDevice::NullDevice(Mixer& mixer, bool realtime, const DeviceOptions& options)
    : impl_{std::make_unique<Impl>(mixer, realtime, options)} {}

NullDevice::~NullDevice() = default;

std::string_view NullDevice::name() const noexcept {
    return "Null-Gerät";
}

u32 NullDevice::sample_rate() const noexcept {
    return impl_->sample_rate;
}

bool NullDevice::realtime() const noexcept {
    return impl_->realtime;
}

std::span<const f32> NullDevice::render(u32 frames) {
    verify(!impl_->realtime, "NullDevice::render() nur ohne Echtzeit-Thread");
    impl_->output.assign(usize{frames} * output_channels, 0.0f);
    impl_->mixer->render(impl_->output);
    return impl_->output;
}

} // namespace ngine::audio
