module;

#include <cstddef>
#include <memory_resource>

export module ngine.memory:linear;

import ngine.core;
import :budget;

export namespace ngine::memory {

// Schiebt bei jeder Allokation nur einen Zeiger weiter. Einzelnes Freigeben tut nichts; reset()
// oder rewind() geben alles bzw. alles ab einer Marke auf einmal frei. Nicht threadsicher.
// Der ganze Speicher wird beim Erzeugen von upstream geholt und als belegt verbucht.
// Reicht er nicht, bricht das Programm mit einer Meldung ab: Die Kapazität ist dann falsch geplant.
class LinearAllocator final : public std::pmr::memory_resource {
public:
    LinearAllocator(usize capacity, Category category,
                    std::pmr::memory_resource* upstream = std::pmr::new_delete_resource());
    ~LinearAllocator() override;
    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    void reset() noexcept {
        offset_ = 0;
    }
    // Marke für rewind(): alles, was danach angefordert wurde, lässt sich gemeinsam freigeben.
    [[nodiscard]] usize marker() const noexcept {
        return offset_;
    }
    void rewind(usize marker);

    [[nodiscard]] usize used() const noexcept {
        return offset_;
    }
    [[nodiscard]] usize capacity() const noexcept {
        return capacity_;
    }

private:
    void* do_allocate(usize bytes, usize alignment) override;
    void do_deallocate(void*, usize, usize) override {}
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::byte* buffer_;
    usize capacity_;
    usize offset_ = 0;
    Category category_;
    std::pmr::memory_resource* upstream_;
};

} // namespace ngine::memory
