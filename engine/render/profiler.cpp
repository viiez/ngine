module;

#include <chrono>
#include <string_view>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.profile;
import ngine.rhi;

namespace ngine::render {

GpuProfiler::GpuProfiler(rhi::Device& device) {
    const f32 period = device.capabilities().timestamp_period;
    if (!profile::enabled || period <= 0.0f) {
        return;
    }
    device_ = &device;
    const auto calibrated = device.calibrated_timestamp();
    context_ = profile::gpu_context("GPU", static_cast<i64>(device.gpu_timestamp()), period,
                                    calibrated.has_value());
    if (calibrated) {
        calibrated_cpu_ns_ = calibrated->cpu_ns;
    }
    active_ = true;
}

void GpuProfiler::recalibrate() {
    // Alle fünf Sekunden; die Uhren laufen nur langsam auseinander.
    constexpr u64 interval_ns = 5'000'000'000;
    const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
    if (now - *calibrated_cpu_ns_ < interval_ns) {
        return;
    }
    const auto sample = device_->calibrated_timestamp();
    if (!sample || sample->cpu_ns <= *calibrated_cpu_ns_) {
        return;
    }
    profile::gpu_calibration(context_, static_cast<i64>(sample->gpu),
                             static_cast<i64>(sample->cpu_ns - *calibrated_cpu_ns_));
    calibrated_cpu_ns_ = sample->cpu_ns;
}

u16 GpuProfiler::query(u32 index) const noexcept {
    return static_cast<u16>(slot_ * rhi::max_frame_timestamps + index);
}

void GpuProfiler::begin_frame(const rhi::Frame& frame) {
    slot_ = frame.index;
    next_ = 0;
    open_.clear();
    in_frame_ = true;
    if (!active_) {
        return;
    }
    for (u32 i = 0; i < frame.timestamps.size(); ++i) {
        profile::gpu_time(context_, query(i), static_cast<i64>(frame.timestamps[i]));
    }
    if (calibrated_cpu_ns_) {
        recalibrate();
    }
}

void GpuProfiler::begin_zone(rhi::CommandList& commands, std::string_view name) {
    // Platz für Beginn und Ende; was nicht mehr passt, bleibt ohne Zone.
    const bool record = active_ && in_frame_ && next_ + 2 <= rhi::max_frame_timestamps;
    open_.push_back(record);
    if (!record) {
        return;
    }
    profile::gpu_zone_begin(context_, query(next_), name);
    commands.write_timestamp(next_++);
}

void GpuProfiler::end_zone(rhi::CommandList& commands) {
    if (open_.empty()) {
        return;
    }
    const bool record = open_.back();
    open_.pop_back();
    if (!record) {
        return;
    }
    profile::gpu_zone_end(context_, query(next_));
    commands.write_timestamp(next_++);
}

} // namespace ngine::render
