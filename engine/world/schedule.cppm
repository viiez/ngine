module;

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

export module ngine.world:schedule;

import ngine.core;
import ngine.jobs;
import :ecs;
import :commands;

export namespace ngine::world {

namespace detail {

// Ein System ohne Typen: Abfrage und eine Funktion für einen Zeilenbereich eines Chunks.
class System {
public:
    virtual ~System() = default;
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // commands ist nur bei Systemen gesetzt, die einen Befehlspuffer nehmen (uses_commands()).
    virtual void run_rows(const Chunk& chunk, u32 begin, u32 end, Commands* commands) = 0;

    [[nodiscard]] const QueryBase& query() const noexcept {
        return query_;
    }
    [[nodiscard]] bool uses_commands() const noexcept {
        return uses_commands_;
    }

protected:
    System(QueryBase query, bool uses_commands) noexcept
        : query_{std::move(query)}, uses_commands_{uses_commands} {}

    QueryBase query_;
    bool uses_commands_;
};

// Ob ein System einen Befehlspuffer als ersten Parameter nimmt.
template <typename F, typename... Ts>
inline constexpr bool takes_commands = std::is_invocable_v<F&, Commands&, Entity, Ts&...> ||
                                       std::is_invocable_v<F&, Commands&, Ts&...>;

template <typename... Ts, typename F, usize... I>
void invoke_row_with_commands(F& function, Commands& commands, World& world, const Chunk& chunk,
                              u32 row, std::index_sequence<I...> /*indices*/) {
    if constexpr (std::is_invocable_v<F&, Commands&, Entity, Ts&...>) {
        function(commands, Entity{world, chunk.entities[row]},
                 static_cast<Ts*>(chunk.columns[I])[row]...);
    } else {
        function(commands, static_cast<Ts*>(chunk.columns[I])[row]...);
    }
}

template <typename F, typename... Ts>
class TypedSystem final : public System {
public:
    TypedSystem(World& world, F function)
        : System{QueryBase{world, make_terms<Ts...>(world)}, takes_commands<F, Ts...>},
          function_{std::move(function)} {}

    void run_rows(const Chunk& chunk, u32 begin, u32 end, Commands* commands) override {
        World& world = query_.world();
        for (u32 row = begin; row < end; ++row) {
            if constexpr (takes_commands<F, Ts...>) {
                invoke_row_with_commands<Ts...>(function_, *commands, world, chunk, row,
                                                std::index_sequence_for<Ts...>{});
            } else {
                invoke_row<Ts...>(function_, world, chunk, row, std::index_sequence_for<Ts...>{});
            }
        }
    }

private:
    F function_;
};

} // namespace detail

// Phasen eines Bildes (WORLD-10, ADR 0044). Die Spielschleife (Simulation) führt sie in dieser
// Reihenfolge aus: Fixed keinmal oder mehrmals mit festem Zeitschritt, dann Frame und Late einmal.
enum class Phase : u8 {
    // Simulation mit festem Zeitschritt: Bewegung, Physik, Gameplay. Reproduzierbar.
    Fixed,
    // Einmal pro Bild mit der Bildzeit: Kamera, Darstellung, was nicht simuliert wird.
    Frame,
    // Nach Frame, vor den Transforms: was die Ergebnisse von Frame braucht.
    Late,
};
inline constexpr usize phase_count = 3;

struct SystemOptions {
    // 0: Das System läuft als ein Job. Sonst teilt es seine Entities in Stücke dieser Größe auf,
    // die parallel laufen. Lohnt sich ab einigen tausend Entities.
    usize grain = 0;
    Phase phase = Phase::Frame;
    // Name eines früher hinzugefügten Systems derselben Phase, nach dem dieses läuft, auch wenn
    // sich ihre Zugriffe nicht überschneiden.
    std::string_view after = {};
};

// Plant Systeme und führt sie auf dem Job-System aus (ADR 0019).
//
// Ein System ist eine Funktion über einer Abfrage, z. B.
//   schedule.add<const Velocity, Position>("Bewegen", [](const Velocity& v, Position& p) { ... });
// Aus const ergibt sich, was ein System liest und was es schreibt. Schreibt ein System, was ein
// früher hinzugefügtes liest oder schreibt (oder umgekehrt), läuft es danach; alle anderen laufen
// parallel. Die Reihenfolge von add() ist also die Reihenfolge bei Konflikten.
//
// Jedes System gehört zu einer Phase (SystemOptions::phase, ohne Angabe Phase::Frame); run()
// führt die Systeme einer Phase aus. Abhängigkeiten gibt es nur innerhalb einer Phase.
//
// Während run() dürfen Systeme nur ihre Komponenten ändern. Entities anlegen oder löschen und
// Komponenten hinzufügen oder entfernen geht über den Befehlspuffer: Nimmt die Funktion Commands&
// als ersten Parameter, sammelt sie Befehle, die run() am Ende in fester Reihenfolge ausführt.
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

    // Führt alle Systeme der Phase einmal aus, wartet, bis sie fertig sind, und führt danach die
    // Befehlspuffer aus (sortiert nach System, Chunk und Zeile).
    void run(jobs::JobSystem& jobs, Phase phase = Phase::Frame);

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
