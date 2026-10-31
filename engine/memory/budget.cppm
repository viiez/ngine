module;

#include <memory_resource>
#include <string_view>

export module ngine.memory:budget;

import ngine.core;

export namespace ngine::memory {

// Wofür Speicher gebraucht wird. Jede Allokation eines Engine-Allokators zählt zu genau einer
// Kategorie; so sehen wir, welches System wie viel verbraucht.
enum class Category : u8 {
    General,
    Rendering,
    World,
    Jobs,
    Scripting,
    Network,
    Audio,
    Tools,
};
inline constexpr usize category_count = 8;

[[nodiscard]] std::string_view to_string(Category category) noexcept;

struct Stats {
    // Belegte Bytes jetzt und höchstens seit dem Start.
    usize current = 0;
    usize peak = 0;
    // 0 bedeutet: kein Budget.
    usize budget = 0;
    // Anzahl der Allokationen seit dem Start.
    u64 allocations = 0;
};

// Legt das Budget einer Kategorie fest. Wird es überschritten, warnt das Log einmal, bis der
// Verbrauch wieder darunter liegt. 0 schaltet das Budget ab.
void set_budget(Category category, usize bytes) noexcept;
[[nodiscard]] Stats stats(Category category) noexcept;

// Für Allokatoren: verbuchen belegten bzw. freigegebenen Speicher. Threadsicher.
void record_allocation(Category category, usize bytes) noexcept;
void record_deallocation(Category category, usize bytes) noexcept;

// Reicht Allokationen an upstream weiter und verbucht sie in einer Kategorie. Damit lassen
// sich Standardcontainer (std::pmr::vector usw.) einer Kategorie zuordnen.
class TrackingResource final : public std::pmr::memory_resource {
public:
    explicit TrackingResource(Category category, std::pmr::memory_resource* upstream =
                                                     std::pmr::new_delete_resource()) noexcept
        : category_{category}, upstream_{upstream} {}

    [[nodiscard]] Category category() const noexcept {
        return category_;
    }

private:
    void* do_allocate(usize bytes, usize alignment) override {
        void* memory = upstream_->allocate(bytes, alignment);
        record_allocation(category_, bytes);
        return memory;
    }
    void do_deallocate(void* pointer, usize bytes, usize alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
        record_deallocation(category_, bytes);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    Category category_;
    std::pmr::memory_resource* upstream_;
};

} // namespace ngine::memory
