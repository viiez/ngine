module;

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <vector>

module ngine.memory;

import ngine.core;

namespace ngine::memory {

PoolAllocator::PoolAllocator(usize block_size, usize block_alignment, usize blocks_per_chunk,
                             Category category, std::pmr::memory_resource* upstream)
    : block_alignment_{std::max(block_alignment, alignof(FreeBlock))},
      blocks_per_chunk_{blocks_per_chunk}, category_{category}, upstream_{upstream} {
    verify(is_power_of_two(block_alignment), "PoolAllocator: Ausrichtung ist keine Zweierpotenz");
    verify(blocks_per_chunk > 0, "PoolAllocator braucht mindestens einen Block pro Chunk");
    // Freie Blöcke speichern den Zeiger der Freiliste in sich selbst.
    block_size_ = align_up(std::max(block_size, sizeof(FreeBlock)), block_alignment_);
}

PoolAllocator::~PoolAllocator() {
    if (blocks_in_use_ != 0) {
        log::warn("PoolAllocator ({}) zerstört, {} Blöcke sind noch belegt", to_string(category_),
                  blocks_in_use_);
    }
    for (void* chunk : chunks_) {
        upstream_->deallocate(chunk, chunk_bytes(), block_alignment_);
    }
    record_deallocation(category_, chunks_.size() * chunk_bytes());
}

void PoolAllocator::grow() {
    auto* chunk = static_cast<std::byte*>(upstream_->allocate(chunk_bytes(), block_alignment_));
    chunks_.push_back(chunk);
    record_allocation(category_, chunk_bytes());
    // Rückwärts einketten, damit Blöcke in aufsteigender Adressreihenfolge herauskommen.
    for (usize i = blocks_per_chunk_; i > 0; --i) {
        auto* block = reinterpret_cast<FreeBlock*>(chunk + (i - 1) * block_size_);
        block->next = free_list_;
        free_list_ = block;
    }
}

void* PoolAllocator::allocate_block() {
    if (free_list_ == nullptr) {
        grow();
    }
    FreeBlock* block = free_list_;
    free_list_ = block->next;
    ++blocks_in_use_;
    return block;
}

void PoolAllocator::deallocate_block(void* block) noexcept {
    auto* free_block = static_cast<FreeBlock*>(block);
    free_block->next = free_list_;
    free_list_ = free_block;
    --blocks_in_use_;
}

void* PoolAllocator::do_allocate(usize bytes, usize alignment) {
    verify(bytes <= block_size_ && alignment <= block_alignment_,
           "PoolAllocator: Anforderung passt nicht in einen Block");
    return allocate_block();
}

void PoolAllocator::do_deallocate(void* pointer, usize, usize) {
    deallocate_block(pointer);
}

} // namespace ngine::memory
