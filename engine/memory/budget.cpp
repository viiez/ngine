module;

#include <array>
#include <atomic>
#include <string_view>

module ngine.memory;

import ngine.core;

namespace ngine::memory {

namespace {

struct Counters {
    std::atomic<usize> current{0};
    std::atomic<usize> peak{0};
    std::atomic<usize> budget{0};
    std::atomic<u64> allocations{0};
    // Verhindert, dass jede weitere Allokation über dem Budget erneut warnt.
    std::atomic<bool> over_budget{false};
};

std::array<Counters, category_count> counters;

Counters& counters_of(Category category) noexcept {
    return counters[static_cast<usize>(category)];
}

} // namespace

std::string_view to_string(Category category) noexcept {
    switch (category) {
    case Category::General: return "Allgemein";
    case Category::Rendering: return "Rendering";
    case Category::World: return "Welt";
    case Category::Jobs: return "Jobs";
    case Category::Scripting: return "Scripting";
    case Category::Network: return "Netzwerk";
    case Category::Audio: return "Audio";
    case Category::Tools: return "Werkzeuge";
    }
    return "unbekannt";
}

void set_budget(Category category, usize bytes) noexcept {
    Counters& c = counters_of(category);
    c.budget.store(bytes, std::memory_order_relaxed);
    c.over_budget.store(false, std::memory_order_relaxed);
}

Stats stats(Category category) noexcept {
    const Counters& c = counters_of(category);
    return Stats{
        .current = c.current.load(std::memory_order_relaxed),
        .peak = c.peak.load(std::memory_order_relaxed),
        .budget = c.budget.load(std::memory_order_relaxed),
        .allocations = c.allocations.load(std::memory_order_relaxed),
    };
}

void record_allocation(Category category, usize bytes) noexcept {
    Counters& c = counters_of(category);
    const usize current = c.current.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    c.allocations.fetch_add(1, std::memory_order_relaxed);

    usize peak = c.peak.load(std::memory_order_relaxed);
    while (current > peak &&
           !c.peak.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {
    }

    const usize budget = c.budget.load(std::memory_order_relaxed);
    if (budget != 0 && current > budget &&
        !c.over_budget.exchange(true, std::memory_order_relaxed)) {
        log::warn("Speicherbudget {} überschritten: {} von {} Bytes", to_string(category), current,
                  budget);
    }
}

void record_deallocation(Category category, usize bytes) noexcept {
    Counters& c = counters_of(category);
    const usize current = c.current.fetch_sub(bytes, std::memory_order_relaxed) - bytes;
    const usize budget = c.budget.load(std::memory_order_relaxed);
    if (budget == 0 || current <= budget) {
        c.over_budget.store(false, std::memory_order_relaxed);
    }
}

} // namespace ngine::memory
