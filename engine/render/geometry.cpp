module;

#include <algorithm>
#include <expected>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.rhi;

namespace ngine::render {

namespace {

constexpr u64 align_up(u64 value, u64 alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

// --- RangeAllocator -------------------------------------------------------------------------

RangeAllocator::RangeAllocator(u64 capacity) : capacity_{capacity} {
    if (capacity > 0) {
        free_.emplace(0, capacity);
    }
}

std::optional<u64> RangeAllocator::allocate(u64 size, u64 alignment) {
    verify(alignment > 0 && (alignment & (alignment - 1)) == 0,
           "RangeAllocator: Ausrichtung muss eine Zweierpotenz sein");
    if (size == 0) {
        return std::nullopt;
    }
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        const u64 begin = it->first;
        const u64 end = begin + it->second;
        const u64 start = align_up(begin, alignment);
        if (start + size > end) {
            continue;
        }
        free_.erase(it);
        if (start > begin) {
            free_.emplace(begin, start - begin);
        }
        if (start + size < end) {
            free_.emplace(start + size, end - start - size);
        }
        used_ += size;
        return start;
    }
    return std::nullopt;
}

void RangeAllocator::free(u64 offset, u64 size) {
    if (size == 0) {
        return;
    }
    verify(offset + size <= capacity_, "RangeAllocator::free: Bereich außerhalb");
    auto [it, inserted] = free_.emplace(offset, size);
    verify(inserted, "RangeAllocator::free: Bereich ist schon frei");
    used_ -= size;
    // Mit dem Nachfolger verschmelzen.
    if (const auto next = std::next(it); next != free_.end()) {
        verify(offset + size <= next->first, "RangeAllocator::free: Bereich überlappt");
        if (offset + size == next->first) {
            it->second += next->second;
            free_.erase(next);
        }
    }
    // Mit dem Vorgänger verschmelzen.
    if (it != free_.begin()) {
        const auto previous = std::prev(it);
        verify(previous->first + previous->second <= offset,
               "RangeAllocator::free: Bereich überlappt");
        if (previous->first + previous->second == offset) {
            previous->second += it->second;
            free_.erase(it);
        }
    }
}

u64 RangeAllocator::largest_free() const noexcept {
    u64 largest = 0;
    for (const auto& entry : free_) {
        largest = std::max(largest, entry.second);
    }
    return largest;
}

// --- GeometryBuffer -------------------------------------------------------------------------

GeometryBuffer::GeometryBuffer(rhi::Device& device, u64 block_size)
    : device_{&device}, block_size_{block_size} {}

rhi::Result<GeometryRange> GeometryBuffer::allocate(u64 size, u64 alignment) {
    for (usize i = 0; i < blocks_.size(); ++i) {
        if (const auto offset = blocks_[i].ranges.allocate(size, alignment)) {
            return GeometryRange{.block = static_cast<u32>(i), .offset = *offset, .size = size};
        }
    }
    // Neuer Block; ein Netz, das größer ist als ein Block, bekommt einen eigenen.
    const u64 capacity = std::max(block_size_, align_up(size, u64{1} << 20));
    auto buffer =
        device_->create_buffer({.size = capacity,
                                .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Index,
                                .memory = rhi::MemoryUsage::Gpu});
    if (!buffer) {
        return std::unexpected("Geometriepuffer: " + buffer.error());
    }
    blocks_.push_back({*std::move(buffer), RangeAllocator{capacity}});
    const auto offset = blocks_.back().ranges.allocate(size, alignment);
    verify(offset.has_value(), "GeometryBuffer: neuer Block zu klein");
    return GeometryRange{
        .block = static_cast<u32>(blocks_.size() - 1), .offset = *offset, .size = size};
}

void GeometryBuffer::free(GeometryRange range) {
    if (range.valid()) {
        pending_.push_back({range, frame_});
    }
}

void GeometryBuffer::end_frame() {
    ++frame_;
    // Ein Frame mehr als im Flug: Auch Kopien auf der Transfer-Queue, die nicht mit dem Zeichnen
    // synchronisiert sind, treffen nur Bereiche, die kein Frame mehr liest.
    const u64 delay = u64{std::max(device_->frames_in_flight(), 1u)} + 2;
    std::erase_if(pending_, [&](const Pending& pending) {
        if (pending.frame + delay > frame_) {
            return false;
        }
        blocks_[pending.range.block].ranges.free(pending.range.offset, pending.range.size);
        return true;
    });
}

void GeometryBuffer::upload(GeometryRange range, std::span<const std::byte> data) {
    verify(range.valid() && data.size() <= range.size, "GeometryBuffer::upload: Bereich zu klein");
    device_->upload(*blocks_[range.block].buffer, range.offset, data);
}

rhi::UploadTicket GeometryBuffer::upload_async(GeometryRange range,
                                               std::span<const std::byte> data) {
    verify(range.valid() && data.size() <= range.size,
           "GeometryBuffer::upload_async: Bereich zu klein");
    return device_->upload_async(*blocks_[range.block].buffer, range.offset, data);
}

rhi::Buffer& GeometryBuffer::buffer(GeometryRange range) const {
    verify(range.valid() && range.block < blocks_.size(), "GeometryBuffer: ungültiger Bereich");
    return *blocks_[range.block].buffer;
}

u64 GeometryBuffer::address(GeometryRange range) const {
    return buffer(range).device_address() + range.offset;
}

GeometryStats GeometryBuffer::stats() const noexcept {
    GeometryStats stats{.blocks = static_cast<u32>(blocks_.size())};
    for (const Block& block : blocks_) {
        stats.capacity += block.ranges.capacity();
        stats.used += block.ranges.used();
        stats.free_ranges += static_cast<u32>(block.ranges.free_ranges());
    }
    return stats;
}

} // namespace ngine::render
