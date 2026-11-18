module;

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.profile;
import ngine.rhi;

namespace ngine::render {

GpuProfiler::GpuProfiler(rhi::Device& device) {
    const f32 period = device.capabilities().timestamp_period;
    if (period <= 0.0f) {
        return;
    }
    device_ = &device;
    period_ns_ = static_cast<f64>(period);
    active_ = true;
    if (!profile::enabled) {
        return;
    }
    tracy_ = true;
    const auto calibrated = device.calibrated_timestamp();
    context_ = profile::gpu_context("GPU", static_cast<i64>(device.gpu_timestamp()), period,
                                    calibrated.has_value());
    if (calibrated) {
        calibrated_cpu_ns_ = calibrated->cpu_ns;
    }
}

f64 GpuProfiler::total_ms() const noexcept {
    f64 total = 0.0;
    for (const PassTime& time : times_) {
        if (time.depth == 0) {
            total += time.milliseconds;
        }
    }
    return total;
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
    if (recorded_.size() <= slot_) {
        recorded_.resize(slot_ + 1);
    }
    // Zeiten der Zonen, die derselbe Platz zuletzt geschrieben hat.
    std::vector<Zone>& zones = recorded_[slot_];
    if (!zones.empty() && !frame.timestamps.empty()) {
        times_.clear();
        for (const Zone& zone : zones) {
            if (zone.end >= frame.timestamps.size() ||
                frame.timestamps[zone.end] < frame.timestamps[zone.begin]) {
                continue;
            }
            const auto ticks =
                static_cast<f64>(frame.timestamps[zone.end] - frame.timestamps[zone.begin]);
            times_.push_back({.name = zone.name,
                              .milliseconds = ticks * period_ns_ / 1.0e6,
                              .depth = zone.depth});
        }
    }
    zones.clear();
    if (tracy_) {
        for (u32 i = 0; i < frame.timestamps.size(); ++i) {
            profile::gpu_time(context_, query(i), static_cast<i64>(frame.timestamps[i]));
        }
        if (calibrated_cpu_ns_) {
            recalibrate();
        }
    }
}

void GpuProfiler::begin_zone(rhi::CommandList& commands, std::string_view name) {
    // Platz für Beginn und Ende; was nicht mehr passt, bleibt ohne Zone.
    const bool record = active_ && in_frame_ && next_ + 2 <= rhi::max_frame_timestamps;
    if (!record) {
        open_.emplace_back();
        return;
    }
    std::vector<Zone>& zones = recorded_[slot_];
    open_.emplace_back(zones.size());
    zones.push_back({.name = std::string{name},
                     .begin = next_,
                     .end = next_,
                     .depth = static_cast<u32>(open_.size() - 1)});
    if (tracy_) {
        profile::gpu_zone_begin(context_, query(next_), name);
    }
    commands.write_timestamp(next_++);
}

void GpuProfiler::end_zone(rhi::CommandList& commands) {
    if (open_.empty()) {
        return;
    }
    const std::optional<usize> zone = open_.back();
    open_.pop_back();
    if (!zone) {
        return;
    }
    recorded_[slot_][*zone].end = next_;
    if (tracy_) {
        profile::gpu_zone_end(context_, query(next_));
    }
    commands.write_timestamp(next_++);
}

std::vector<PassGroup> grouped_pass_times(const GpuProfiler& profiler) {
    std::vector<PassGroup> groups;
    for (const PassTime& time : profiler.pass_times()) {
        if (time.depth != 0) {
            continue;
        }
        auto group = std::ranges::find(groups, time.name, &PassGroup::name);
        if (group == groups.end()) {
            groups.push_back({.name = time.name});
            group = groups.end() - 1;
        }
        group->milliseconds += time.milliseconds;
        ++group->count;
    }
    return groups;
}

std::string pass_report(const GpuProfiler& profiler) {
    if (!profiler.active()) {
        return "GPU-Zeiten: keine Zeitstempel auf diesem Gerät";
    }
    if (profiler.pass_times().empty()) {
        return "GPU-Zeiten: noch keine";
    }
    std::string text = std::format("GPU {:.2f} ms:", profiler.total_ms());
    bool first = true;
    for (const PassGroup& group : grouped_pass_times(profiler)) {
        text += std::format("{} {} {:.2f}", first ? "" : ",", group.name, group.milliseconds);
        if (group.count > 1) {
            text += std::format(" ({}×)", group.count);
        }
        first = false;
    }
    return text;
}

} // namespace ngine::render
