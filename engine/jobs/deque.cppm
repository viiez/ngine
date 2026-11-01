module;

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

export module ngine.jobs:deque;

import ngine.core;

export namespace ngine::jobs {

// Lock-freie Work-Stealing-Deque nach Chase und Lev in der Fassung von Lê, Pop, Cohen und
// Zappa Nardelli (2013, „Correct and Efficient Work-Stealing for Weak Memory Models“).
//
// Nur der Besitzer ruft push() und pop() auf und arbeitet am unteren Ende (LIFO, gut für den
// Cache). Beliebige andere Threads stehlen mit steal() vom oberen Ende (FIFO, große Brocken).
// Statt der Zäune aus dem Papier nutzen wir seq_cst-Zugriffe: auf x86 gleich teuer, und
// ThreadSanitizer versteht sie (Zäune nicht).
//
// T muss ein Zeiger sein; nullptr bedeutet „leer“. Bei Bedarf wächst der Puffer. Alte Puffer
// bleiben bis zum Ende leben, weil ein Dieb sie noch lesen kann.
template <typename T>
    requires std::is_pointer_v<T>
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(i64 capacity = 256) {
        auto buffer = std::make_unique<Buffer>(capacity);
        buffer_.store(buffer.get(), std::memory_order_relaxed);
        buffers_.push_back(std::move(buffer));
    }
    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

    // Nur der Besitzer.
    void push(T item) {
        const i64 bottom = bottom_.load(std::memory_order_relaxed);
        const i64 top = top_.load(std::memory_order_acquire);
        Buffer* buffer = buffer_.load(std::memory_order_relaxed);
        if (bottom - top > buffer->capacity - 1) {
            buffer = grow(buffer, bottom, top);
        }
        buffer->store(bottom, item);
        // Veröffentlicht das Element samt allem, was vorher in es geschrieben wurde.
        bottom_.store(bottom + 1, std::memory_order_release);
    }

    // Nur der Besitzer. Liefert nullptr, wenn die Deque leer ist.
    [[nodiscard]] T pop() {
        const i64 bottom = bottom_.load(std::memory_order_relaxed) - 1;
        Buffer* buffer = buffer_.load(std::memory_order_relaxed);
        // Store-Load-Ordnung zu top: entspricht dem seq_cst-Zaun im Papier.
        bottom_.store(bottom, std::memory_order_seq_cst);
        i64 top = top_.load(std::memory_order_seq_cst);

        if (top > bottom) {
            // Leer.
            bottom_.store(bottom + 1, std::memory_order_relaxed);
            return nullptr;
        }
        T item = buffer->load(bottom);
        if (top == bottom) {
            // Letztes Element: Wettlauf mit Dieben über top entscheiden.
            if (!top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
                item = nullptr;
            }
            bottom_.store(bottom + 1, std::memory_order_relaxed);
        }
        return item;
    }

    // Beliebiger Thread. Liefert nullptr, wenn nichts da ist oder ein anderer schneller war.
    [[nodiscard]] T steal() {
        i64 top = top_.load(std::memory_order_seq_cst);
        const i64 bottom = bottom_.load(std::memory_order_seq_cst);
        if (top >= bottom) {
            return nullptr;
        }
        Buffer* buffer = buffer_.load(std::memory_order_acquire);
        T item = buffer->load(top);
        if (!top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
            return nullptr;
        }
        return item;
    }

    // Ungefähre Größe; nur für Statistik und Tests.
    [[nodiscard]] i64 size() const {
        const i64 bottom = bottom_.load(std::memory_order_relaxed);
        const i64 top = top_.load(std::memory_order_relaxed);
        return bottom > top ? bottom - top : 0;
    }

private:
    struct Buffer {
        explicit Buffer(i64 size)
            : capacity{size}, mask{size - 1},
              items{std::make_unique<std::atomic<T>[]>(static_cast<usize>(size))} {
            verify(size > 0 && (size & (size - 1)) == 0,
                   "WorkStealingDeque: Kapazität muss eine Zweierpotenz sein");
        }
        [[nodiscard]] T load(i64 index) const {
            return items[static_cast<usize>(index & mask)].load(std::memory_order_relaxed);
        }
        void store(i64 index, T item) {
            items[static_cast<usize>(index & mask)].store(item, std::memory_order_relaxed);
        }

        i64 capacity;
        i64 mask;
        std::unique_ptr<std::atomic<T>[]> items;
    };

    Buffer* grow(Buffer* old, i64 bottom, i64 top) {
        auto bigger = std::make_unique<Buffer>(old->capacity * 2);
        for (i64 i = top; i < bottom; ++i) {
            bigger->store(i, old->load(i));
        }
        Buffer* raw = bigger.get();
        buffers_.push_back(std::move(bigger));
        buffer_.store(raw, std::memory_order_release);
        return raw;
    }

    // Eigene Cache-Zeilen, damit Besitzer und Diebe sich nicht gegenseitig ausbremsen.
    alignas(64) std::atomic<i64> top_{0};
    alignas(64) std::atomic<i64> bottom_{0};
    alignas(64) std::atomic<Buffer*> buffer_{nullptr};
    // Gehört dem Besitzer.
    std::vector<std::unique_ptr<Buffer>> buffers_;
};

} // namespace ngine::jobs
