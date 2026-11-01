module;

#include <atomic>
#include <cstddef>
#include <memory_resource>

export module ngine.memory:frame;

import ngine.core;
import :budget;

export namespace ngine::memory {

// Speicher für Daten, die nur einen Frame leben. Es gibt einen Bereich pro Frame im Flug,
// damit Daten gültig bleiben, solange die GPU den Frame noch bearbeitet. next_frame() wechselt
// zum nächsten Bereich und gibt ihn frei.
//
// Anfordern ist threadsicher und lock-frei (ein atomarer Zeiger pro Bereich), Jobs dürfen also
// gleichzeitig allozieren. next_frame() darf nicht parallel zu Allokationen laufen.
class FrameAllocator final : public std::pmr::memory_resource {
public:
    FrameAllocator(usize capacity_per_frame, u32 frames_in_flight, Category category,
                   std::pmr::memory_resource* upstream = std::pmr::new_delete_resource());
    ~FrameAllocator() override;
    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;

    void next_frame() noexcept;

    [[nodiscard]] u32 frame_index() const noexcept {
        return frame_index_;
    }
    // Belegte Bytes im aktuellen Frame.
    [[nodiscard]] usize used() const noexcept {
        return offset_.load(std::memory_order_relaxed);
    }
    // Höchster Füllstand eines Frames seit dem Start; hilft, die Kapazität zu planen.
    [[nodiscard]] usize peak() const noexcept {
        return peak_;
    }
    [[nodiscard]] usize capacity_per_frame() const noexcept {
        return capacity_per_frame_;
    }

private:
    void* do_allocate(usize bytes, usize alignment) override;
    void do_deallocate(void*, usize, usize) override {}
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    [[nodiscard]] std::byte* current_base() const noexcept {
        return buffer_ + static_cast<usize>(frame_index_) * capacity_per_frame_;
    }

    std::byte* buffer_;
    usize capacity_per_frame_;
    u32 frames_in_flight_;
    u32 frame_index_ = 0;
    std::atomic<usize> offset_{0};
    usize peak_ = 0;
    Category category_;
    std::pmr::memory_resource* upstream_;
};

} // namespace ngine::memory
