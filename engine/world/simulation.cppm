module;

#include <functional>
#include <string_view>
#include <utility>
#include <vector>

export module ngine.world:simulation;

import ngine.core;
import ngine.jobs;
import :ecs;
import :transform;
import :schedule;

export namespace ngine::world {

// Entities mit dieser Komponente werden zwischen den letzten beiden festen Schritten gemischt
// gezeichnet (Interpolation): WorldTransform zeigt die Lage zum Zeitpunkt alpha, Transform bleibt
// die Lage der Simulation. Ohne die Komponente springt eine Entity, die sich in Phase::Fixed
// bewegt, von Schritt zu Schritt. Gespeichert wird nur, dass eine Entity sie hat; die Felder sind
// Zustand der Spielschleife.
struct Interpolated {
    // Transform vor dem letzten festen Schritt.
    Transform previous;
    // Lage der Simulation beim letzten Mischen.
    Transform simulated;
    bool initialized = false;
    // WorldTransform hält eine gemischte Lage.
    bool mixed = false;
};

struct SimulationOptions {
    // Länge eines festen Schritts in Sekunden.
    f64 fixed_step = 1.0 / 60.0;
    // Höchstzahl fester Schritte pro Bild. Was darüber hinausgeht, verfällt: Unter
    // fixed_step × max_steps Bildern pro Sekunde läuft das Spiel langsamer, statt immer mehr
    // Schritte nach sich zu ziehen.
    u32 max_steps = 4;
};

// Zeiten des letzten Bildes in Millisekunden (für Messungen, R4.2).
struct SimulationStats {
    u32 steps = 0;
    // Alle festen Schritte samt Stufen (Physik).
    f64 fixed_ms = 0.0;
    // Phase::Frame und Phase::Late.
    f64 frame_ms = 0.0;
    // update_transforms() samt Interpolation, und wie viele Entities es neu berechnet hat.
    f64 transforms_ms = 0.0;
    usize transforms = 0;
};

// Spielschleife (WORLD-10, ADR 0044): besitzt den Schedule und eine Uhr und treibt die Welt pro
// Bild durch die Phasen. advance(dt) rechnet so viele feste Schritte, wie in die angesammelte Zeit
// passen (Phase::Fixed, nach jedem Schritt die Befehlspuffer), dann einmal Phase::Frame und
// Phase::Late, dann update_transforms() und die Interpolation. Sandbox, Editor und Server nutzen
// dieselbe Schleife; ohne Fenster und GPU läuft sie genauso.
class Simulation {
public:
    Simulation(World& world, jobs::JobSystem& jobs, SimulationOptions options = {});
    ~Simulation();
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;
    Simulation(Simulation&&) = delete;
    Simulation& operator=(Simulation&&) = delete;

    // Fügt ein System hinzu (siehe Schedule::add()); die Phase steht in options.
    template <typename... Ts, typename F>
    void add(std::string_view name, F&& function, SystemOptions options = {}) {
        schedule_.add<Ts...>(name, std::forward<F>(function), options);
    }

    [[nodiscard]] Schedule& schedule() noexcept {
        return schedule_;
    }
    [[nodiscard]] World& world() noexcept {
        return *world_;
    }
    [[nodiscard]] const Time& time() const noexcept {
        return world_->time();
    }

    // Rechnet ein Bild, das dt Sekunden gedauert hat. Liefert die Zahl der festen Schritte.
    u32 advance(f64 dt);

    // Pause hält die Simulation an; Phase::Frame läuft weiter mit Time::delta = 0.
    void set_paused(bool paused) noexcept;
    [[nodiscard]] bool paused() const noexcept {
        return paused_;
    }
    // Zeitlupe (unter 1) oder Zeitraffer (über 1). Ändert, wie viel Zeit pro Bild anfällt, nicht
    // die Länge eines festen Schritts.
    void set_time_scale(f64 scale) noexcept;
    [[nodiscard]] f64 time_scale() const noexcept {
        return time_scale_;
    }
    // Das nächste advance() rechnet genau einen festen Schritt, auch in der Pause.
    void step() noexcept {
        step_requested_ = true;
    }
    // Setzt die Uhr auf null (z. B. beim Stoppen des Spielmodus); der Schedule bleibt.
    void reset_clock() noexcept;

    // Stufe nach den Systemen jedes festen Schritts, nach ihren Befehlspuffern und vor
    // set_after_fixed_step(): Arbeit an der ganzen Welt, die kein System pro Entity ist und die
    // Struktur ändern darf, z. B. der Schritt der Physik (R4.2). Stufen laufen in der Reihenfolge
    // ihres Hinzufügens im Hauptthread.
    void add_fixed_stage(std::function<void()> stage) {
        fixed_stages_.push_back(std::move(stage));
    }

    // Wird nach jedem festen Schritt aufgerufen, nachdem die Befehlspuffer ausgeführt sind. Die
    // Eingabe gibt darüber ihre Flanken frei (input::ActionState::end_fixed_step()).
    void set_after_fixed_step(std::function<void()> callback) {
        after_fixed_step_ = std::move(callback);
    }

    [[nodiscard]] const SimulationOptions& options() const noexcept {
        return options_;
    }
    [[nodiscard]] const SimulationStats& stats() const noexcept {
        return stats_;
    }

private:
    void fixed_step();
    void interpolate();

    World* world_;
    jobs::JobSystem* jobs_;
    SimulationOptions options_;
    Schedule schedule_;
    Query<Interpolated, const Transform> interpolated_;
    std::vector<std::function<void()>> fixed_stages_;
    std::function<void()> after_fixed_step_;
    SimulationStats stats_;
    f64 accumulator_ = 0.0;
    f64 time_scale_ = 1.0;
    bool paused_ = false;
    bool step_requested_ = false;
};

} // namespace ngine::world
