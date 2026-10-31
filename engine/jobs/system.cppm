module;

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

export module ngine.jobs:system;

import ngine.core;
import ngine.memory;
import :deque;
import :function;

export namespace ngine::jobs {
class Counter;
} // namespace ngine::jobs

namespace ngine::jobs::detail {

// Ein Job liegt in einem Pool des Threads, der ihn angelegt hat, und kehrt nach der Ausführung
// dorthin zurück. Zwei Cache-Zeilen groß.
struct alignas(64) Job {
    JobFunction function;
    Counter* counter = nullptr;
    Job* next_free = nullptr;
    u32 owner = 0;
};

struct Slot;

} // namespace ngine::jobs::detail

export namespace ngine::jobs {

// Zählt laufende Jobs. JobSystem::run() erhöht ihn, das Ende eines Jobs verringert ihn;
// JobSystem::wait() wartet, bis er null ist. Muss leben, bis er null ist.
class Counter {
public:
    Counter() = default;
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;

    [[nodiscard]] bool done() const noexcept {
        return value_.load(std::memory_order_acquire) == 0;
    }
    [[nodiscard]] u32 pending() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    friend class JobSystem;
    friend class TaskGraph;
    std::atomic<u32> value_{0};
};

struct JobSystemDesc {
    // Anzahl der Worker-Threads. Ohne Angabe: ein Thread pro Kern, abzüglich des Threads, der
    // das JobSystem erzeugt (er arbeitet beim Warten mit). 0 heißt: alles läuft beim Warten.
    std::optional<u32> worker_count;
};

// Verteilt Jobs auf Worker-Threads mit Work-Stealing.
//
// Jeder Worker und der erzeugende Thread haben eine eigene Deque: Neue Jobs landen vorne in der
// eigenen Deque, untätige Threads stehlen hinten bei anderen. Jobs aus anderen Threads landen in
// einer gemeinsamen Warteschlange. Ohne Arbeit schlafen die Worker.
//
// run() und wait() dürfen aus jedem Thread aufgerufen werden, auch aus Jobs heraus. wait()
// blockiert nicht, sondern arbeitet andere Jobs ab. Der Destruktor arbeitet alle offenen Jobs ab.
class JobSystem {
public:
    explicit JobSystem(const JobSystemDesc& desc = {});
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Startet function als Job. Mit counter lässt sich auf das Ende warten.
    template <typename F>
    void run(F&& function, Counter* counter = nullptr) {
        detail::Job* job = allocate_job();
        job->function.emplace(std::forward<F>(function));
        job->counter = counter;
        if (counter != nullptr) {
            counter->value_.fetch_add(1, std::memory_order_relaxed);
        }
        submit(job);
    }

    // Arbeitet Jobs ab, bis counter null ist.
    void wait(const Counter& counter);

    // Ruft function(begin, end) für Teilbereiche von [0, count) auf, jeder höchstens grain groß,
    // und wartet auf alle. Der Bereich wird rekursiv halbiert, damit sich die Teile beim
    // Stehlen gut verteilen.
    template <typename F>
    void parallel_for(usize count, usize grain, F&& function) {
        Counter counter;
        split(0, count, std::max<usize>(grain, 1), function, counter);
        wait(counter);
    }

    [[nodiscard]] u32 worker_count() const noexcept {
        return worker_count_;
    }

private:
    template <typename F>
    void split(usize begin, usize end, usize grain, F& function, Counter& counter) {
        while (end - begin > grain) {
            const usize middle = begin + (end - begin) / 2;
            run([this, middle, end, grain, &function,
                 &counter] { split(middle, end, grain, function, counter); },
                &counter);
            end = middle;
        }
        if (begin < end) {
            function(begin, end);
        }
    }

    [[nodiscard]] u32 current_slot() const noexcept;
    [[nodiscard]] bool has_deque(u32 slot) const noexcept {
        return slot <= worker_count_;
    }

    [[nodiscard]] detail::Job* allocate_job();
    void free_job(detail::Job* job, u32 slot) noexcept;
    void submit(detail::Job* job);
    void execute(detail::Job* job, u32 slot) noexcept;
    [[nodiscard]] detail::Job* find_job(u32 slot);
    void worker_main(u32 slot);

    u32 worker_count_ = 0;
    // Index external_slot_: alle Threads, die weder Worker sind noch das JobSystem erzeugt haben.
    u32 external_slot_ = 0;
    // 0: erzeugender Thread, 1 bis worker_count_: Worker, external_slot_: alle anderen.
    std::vector<std::unique_ptr<detail::Slot>> slots_;

    std::mutex injection_mutex_;
    std::deque<detail::Job*> injection_;
    std::atomic<usize> injection_size_{0};
    // Schützt das Anlegen von Jobs aus fremden Threads.
    std::mutex external_mutex_;

    // Aufwecken: Jede neue Arbeit erhöht epoch_; schlafende Worker warten auf eine Änderung.
    std::atomic<u32> epoch_{0};
    std::atomic<u32> sleeping_{0};
    std::atomic<bool> stop_{false};

    std::vector<std::jthread> workers_;
    // Zustand des erzeugenden Threads vor dem Konstruktor, wird im Destruktor wiederhergestellt.
    const JobSystem* previous_system_ = nullptr;
    u32 previous_slot_ = 0;
};

} // namespace ngine::jobs
