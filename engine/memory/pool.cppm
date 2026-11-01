module;

#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

export module ngine.memory:pool;

import ngine.core;
import :budget;

export namespace ngine::memory {

// Blöcke fester Größe mit Freiliste: Anfordern und Freigeben in O(1), keine Fragmentierung.
// Wächst in Chunks von blocks_per_chunk Blöcken und gibt Speicher erst im Destruktor zurück.
// Nicht threadsicher.
class PoolAllocator final : public std::pmr::memory_resource {
public:
    PoolAllocator(usize block_size, usize block_alignment, usize blocks_per_chunk,
                  Category category,
                  std::pmr::memory_resource* upstream = std::pmr::new_delete_resource());
    ~PoolAllocator() override;
    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    [[nodiscard]] void* allocate_block();
    void deallocate_block(void* block) noexcept;

    [[nodiscard]] usize block_size() const noexcept {
        return block_size_;
    }
    [[nodiscard]] usize blocks_in_use() const noexcept {
        return blocks_in_use_;
    }
    // Blöcke in allen bisher geholten Chunks.
    [[nodiscard]] usize capacity() const noexcept {
        return chunks_.size() * blocks_per_chunk_;
    }

private:
    struct FreeBlock {
        FreeBlock* next;
    };

    void* do_allocate(usize bytes, usize alignment) override;
    void do_deallocate(void* pointer, usize bytes, usize alignment) override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    void grow();
    [[nodiscard]] usize chunk_bytes() const noexcept {
        return block_size_ * blocks_per_chunk_;
    }

    usize block_size_;
    usize block_alignment_;
    usize blocks_per_chunk_;
    Category category_;
    std::pmr::memory_resource* upstream_;
    FreeBlock* free_list_ = nullptr;
    usize blocks_in_use_ = 0;
    std::vector<void*> chunks_;
};

// Typisierter Pool: erzeugt und zerstört Objekte vom Typ T in Blöcken eines PoolAllocator.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(usize objects_per_chunk, Category category = Category::General,
                        std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
        : pool_{sizeof(T), alignof(T), objects_per_chunk, category, upstream} {}

    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        void* block = pool_.allocate_block();
        // Gibt den Block zurück, falls der Konstruktor eine Ausnahme wirft.
        struct Guard {
            PoolAllocator& pool;
            void* block;
            ~Guard() {
                if (block != nullptr) {
                    pool.deallocate_block(block);
                }
            }
        } guard{pool_, block};
        T* object = std::construct_at(static_cast<T*>(block), std::forward<Args>(args)...);
        guard.block = nullptr;
        return object;
    }

    void destroy(T* object) noexcept {
        std::destroy_at(object);
        pool_.deallocate_block(object);
    }

    [[nodiscard]] usize size() const noexcept {
        return pool_.blocks_in_use();
    }

private:
    PoolAllocator pool_;
};

} // namespace ngine::memory
