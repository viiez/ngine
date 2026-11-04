module;

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
    context_ = profile::gpu_context("GPU", static_cast<i64>(device.gpu_timestamp()), period);
    active_ = true;
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
