module;

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

export module ngine.world:schedule;

import ngine.core;
import ngine.jobs;
import :ecs;

export namespace ngine::world {

namespace detail {

// Ein System ohne Typen: Abfrage und eine Funktion für einen Zeilenbereich eines Chunks.
class System {
public:
    virtual ~System() = default;
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    virtual void run_rows(const Chunk& chunk, u32 begin, u32 end) = 0;

    [[nodiscard]] const QueryBase& query() const noexcept {
        return query_;
    }

protected:
    explicit System(QueryBase query) noexcept : query_{std::move(query)} {}

    QueryBase query_;
};

template <typename F, typename... Ts>
class TypedSystem final : public System {
public:
    TypedSystem(World& world, F function)
        : System{QueryBase{world, make_terms<Ts...>(world)}}, function_{std::move(function)} {}

    void run_rows(const Chunk& chunk, u32 begin, u32 end) override {
        World& world = query_.world();
        for (u32 row = begin; row < end; ++row) {
            invoke_row<Ts...>(function_, world, chunk, row, std::index_sequence_for<Ts...>{});
        }
    }

private:
    F function_;
};

} // namespace detail

struct SystemOptions {
    // 0: Das System läuft als ein Job. Sonst teilt es seine Entities in Stücke dieser Größe auf,
    // die parallel laufen (parallel_for). Lohnt sich ab einigen tausend Entities.
    usize grain = 0;
};

// Plant Systeme und führt sie auf dem Job-System aus (ADR 0019).
//
// Ein System ist eine Funktion über einer Abfrage, z. B.
//   schedule.add<const Velocity, Position>("Bewegen", [](const Velocity& v, Position& p) { ... });
// Aus const ergibt sich, was ein System liest und was es schreibt. Schreibt ein System, was ein
// früher hinzugefügtes liest oder schreibt (oder umgekehrt), läuft es danach; alle anderen laufen
// parallel. Die Reihenfolge von add() ist also die Reihenfolge bei Konflikten.
//
// Während run() dürfen Systeme nur ihre Komponenten ändern. Entities anlegen oder löschen und
// Komponenten hinzufügen oder entfernen bricht ab; solche Änderungen kommen nach run().
class Schedule {
public:
    explicit Schedule(World& world);
    ~Schedule();
    Schedule(const Schedule&) = delete;
    Schedule& operator=(const Schedule&) = delete;

    template <typename... Ts, typename F>
    void add(std::string_view name, F&& function, SystemOptions options = {}) {
        using Stored = std::decay_t<F>;
        add_system(name, options,
                   std::make_unique<detail::TypedSystem<Stored, Ts...>>(
                       *world_, Stored{std::forward<F>(function)}));
    }

    // Führt alle Systeme einmal aus und wartet, bis sie fertig sind.
    void run(jobs::JobSystem& jobs);

    [[nodiscard]] usize size() const noexcept;
    // Ob System later wegen eines Konflikts auf System earlier wartet (Indizes in add-Reihenfolge).
    [[nodiscard]] bool waits_for(usize later, usize earlier) const noexcept;

private:
    void add_system(std::string_view name, SystemOptions options,
                    std::unique_ptr<detail::System> system);

    struct Impl;
    World* world_;
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::world
