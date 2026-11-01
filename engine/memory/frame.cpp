module;

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <format>
#include <memory_resource>

module ngine.memory;

import ngine.core;

namespace ngine::memory {

namespace {
// Eine Cache-Zeile, damit Bereiche verschiedener Frames sich keine Zeile teilen.
constexpr usize buffer_alignment = 64;
} // namespace

FrameAllocator::FrameAllocator(usize capacity_per_frame, u32 frames_in_flight, Category category,
                               std::pmr::memory_resource* upstream)
    : buffer_{nullptr}, capacity_per_frame_{align_up(capacity_per_frame, buffer_alignment)},
      frames_in_flight_{frames_in_flight}, category_{category}, upstream_{upstream} {
    verify(frames_in_flight_ > 0, "FrameAllocator braucht mindestens einen Frame");
    buffer_ = static_cast<std::byte*>(
        upstream_->allocate(capacity_per_frame_ * frames_in_flight_, buffer_alignment));
    record_allocation(category_, capacity_per_frame_ * frames_in_flight_);
}

FrameAllocator::~FrameAllocator() {
    upstream_->deallocate(buffer_, capacity_per_frame_ * frames_in_flight_, buffer_alignment);
    record_deallocation(category_, capacity_per_frame_ * frames_in_flight_);
}

void FrameAllocator::next_frame() noexcept {
    peak_ = std::max(peak_, offset_.load(std::memory_order_relaxed));
    frame_index_ = (frame_index_ + 1) % frames_in_flight_;
    offset_.store(0, std::memory_order_relaxed);
}

void* FrameAllocator::do_allocate(usize bytes, usize alignment) {
    verify(is_power_of_two(alignment), "FrameAllocator: Ausrichtung ist keine Zweierpotenz");
    std::byte* const base = current_base();
    usize offset = offset_.load(std::memory_order_relaxed);
    usize start = 0;
    // Ausrichten und Weiterschieben müssen gemeinsam gelingen, daher CAS statt fetch_add.
    do {
        start = offset + alignment_padding(base + offset, alignment);
        if (start > capacity_per_frame_ || bytes > capacity_per_frame_ - start) [[unlikely]] {
            verify(false,
                   std::format("FrameAllocator ({}) voll: {} Bytes angefordert, {} von {} belegt",
                               to_string(category_), bytes, offset, capacity_per_frame_));
        }
    } while (!offset_.compare_exchange_weak(offset, start + bytes, std::memory_order_relaxed));
    return base + start;
}

} // namespace ngine::memory
