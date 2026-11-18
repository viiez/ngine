module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

export module ngine.audio:queue;

import ngine.core;

// Warteschlangen ohne Sperren mit genau einem Schreiber und einem Leser. Der Misch-Thread darf
// weder sperren noch Speicher anfordern; über diese Puffer reden Hauptthread, Mixer und
// Strom-Thread miteinander.
export namespace ngine::audio {

// Feste Zahl von Einträgen (Zweierpotenz), z. B. Befehle an den Mixer.
template <typename T, usize Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Die Kapazität muss eine Zweierpotenz sein");

public:
    // Nur der Schreiber. false, wenn die Schlange voll ist.
    bool push(const T& item) noexcept {
        const usize tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) == Capacity) {
            return false;
        }
        items_[tail & (Capacity - 1)] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Nur der Leser. false, wenn nichts da ist.
    bool pop(T& item) noexcept {
        const usize head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        item = items_[head & (Capacity - 1)];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] usize size() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    std::array<T, Capacity> items_{};
    alignas(64) std::atomic<usize> head_{0};
    alignas(64) std::atomic<usize> tail_{0};
};

// Ringpuffer für Samples mit fester Größe, angelegt vor der Benutzung (Ströme: Decoder schreibt,
// Mixer liest).
class SampleRing {
public:
    explicit SampleRing(usize capacity) : samples_(capacity) {}

    [[nodiscard]] usize capacity() const noexcept {
        return samples_.size();
    }
    // Wie viele Samples der Leser holen kann.
    [[nodiscard]] usize available() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }
    // Wie viele Samples der Schreiber noch ablegen kann.
    [[nodiscard]] usize free() const noexcept {
        return capacity() - available();
    }

    // Nur der Schreiber: legt so viele Samples ab, wie Platz ist; liefert die Zahl.
    usize write(std::span<const f32> data) noexcept {
        const usize tail = tail_.load(std::memory_order_relaxed);
        const usize head = head_.load(std::memory_order_acquire);
        const usize count = std::min(data.size(), capacity() - (tail - head));
        for (usize i = 0; i < count; ++i) {
            samples_[(tail + i) % capacity()] = data[i];
        }
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // Nur der Leser: holt höchstens out.size() Samples; liefert die Zahl.
    usize read(std::span<f32> out) noexcept {
        const usize head = head_.load(std::memory_order_relaxed);
        const usize tail = tail_.load(std::memory_order_acquire);
        const usize count = std::min(out.size(), tail - head);
        for (usize i = 0; i < count; ++i) {
            out[i] = samples_[(head + i) % capacity()];
        }
        head_.store(head + count, std::memory_order_release);
        return count;
    }

private:
    std::vector<f32> samples_;
    alignas(64) std::atomic<usize> head_{0};
    alignas(64) std::atomic<usize> tail_{0};
};

} // namespace ngine::audio
