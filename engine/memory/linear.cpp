module;

#include <cstddef>
#include <format>
#include <memory_resource>

module ngine.memory;

import ngine.core;

namespace ngine::memory {

namespace {
constexpr usize buffer_alignment = alignof(std::max_align_t);
}

LinearAllocator::LinearAllocator(usize capacity, Category category,
                                 std::pmr::memory_resource* upstream)
    : buffer_{static_cast<std::byte*>(upstream->allocate(capacity, buffer_alignment))},
      capacity_{capacity}, category_{category}, upstream_{upstream} {
    record_allocation(category_, capacity_);
}

LinearAllocator::~LinearAllocator() {
    upstream_->deallocate(buffer_, capacity_, buffer_alignment);
    record_deallocation(category_, capacity_);
}

void LinearAllocator::rewind(usize marker) {
    verify(marker <= offset_, "LinearAllocator::rewind: Marke liegt hinter dem Füllstand");
    offset_ = marker;
}

void* LinearAllocator::do_allocate(usize bytes, usize alignment) {
    verify(is_power_of_two(alignment), "LinearAllocator: Ausrichtung ist keine Zweierpotenz");
    const usize start = offset_ + alignment_padding(buffer_ + offset_, alignment);
    if (start > capacity_ || bytes > capacity_ - start) [[unlikely]] {
        verify(false,
               std::format("LinearAllocator ({}) voll: {} Bytes angefordert, {} von {} belegt",
                           to_string(category_), bytes, offset_, capacity_));
    }
    offset_ = start + bytes;
    return buffer_ + start;
}

} // namespace ngine::memory
