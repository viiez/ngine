module;

#include <deque>
#include <functional>
#include <optional>
#include <vector>

// Vorhersage der eigenen Figur (R5.3, NET-05, ADR 0053), ohne Physik und Netz, damit sie sich
// einzeln testen lässt. Der Client merkt sich jeden Befehl mit dem Zustand, den er danach
// vorhergesagt hat. Meldet der Server seinen Zustand nach einem Befehl, vergleicht der Predictor;
// weicht er mehr als die Toleranz ab, übernimmt er den Stand des Servers und rechnet alle späteren
// Befehle neu. Setzen und Rechnen kommen als Rückrufe (beim Client die Physik).
export module ngine.replication:prediction;

import ngine.core;
import ngine.input;
import ngine.math;
import :input;

export namespace ngine::replication {

// Zustand einer vorhergesagten Figur (wie physics::CharacterSnapshot, ohne von der Physik
// abzuhängen).
struct PredictedState {
    math::DVec3 position{0.0, 0.0, 0.0};
    math::Vec3 velocity{0.0f, 0.0f, 0.0f};
    u8 ground = 0;
    bool crouched = false;
};

struct PredictionSettings {
    // Ab dieser Abweichung der Lage (m) bzw. der Geschwindigkeit (m/s) wird korrigiert.
    f64 position_tolerance = 0.01;
    f32 velocity_tolerance = 0.05f;
    // Gemerkte Befehle (gut eine Sekunde).
    usize history = 64;
};

struct PredictionStats {
    u64 corrections = 0;
    // Neu gerechnete Schritte über alle Korrekturen.
    u64 replayed = 0;
    // Abweichung der Lage bei der letzten Korrektur und beim letzten Vergleich (m).
    f64 last_correction = 0.0;
    f64 last_error = 0.0;
    // Stände vom Server, zu denen der Befehl nicht (mehr) bekannt war.
    u64 unknown = 0;
    // Zuletzt vom Server bestätigter Befehl und wie viele danach noch offen sind.
    u32 acknowledged = 0;
    u32 pending = 0;
};

class Predictor {
public:
    using SetState = std::function<void(const PredictedState&)>;
    using Step = std::function<PredictedState(const Command&)>;

    explicit Predictor(PredictionSettings settings = {}) : settings_{settings} {}

    // Nummer für den nächsten Befehl (ab 1).
    [[nodiscard]] u32 next_number() const noexcept {
        return next_;
    }
    // Nach jedem vorhergesagten Schritt: der Befehl und der Zustand danach.
    void record(Command command, const PredictedState& after);
    // Stand des Servers nach dem Befehl number. Weicht er ab, ruft der Predictor set mit ihm und
    // danach step für jeden späteren Befehl (älteste zuerst) und merkt sich die neuen Zustände.
    // Liefert true bei einer Korrektur.
    bool reconcile(u32 number, const PredictedState& server, const SetState& set, const Step& step);

    // Die neuesten count Befehle zum Senden, älteste zuerst.
    [[nodiscard]] std::vector<Command> recent(usize count) const;
    // Vorhergesagter Zustand nach dem neuesten Befehl.
    [[nodiscard]] std::optional<PredictedState> latest() const;
    [[nodiscard]] PredictionStats stats() const noexcept;
    // Vergisst alles (neue Figur, Trennung); die Nummern zählen weiter.
    void reset();

private:
    struct Entry {
        Command command;
        PredictedState state;
    };

    PredictionSettings settings_;
    std::deque<Entry> entries_;
    u32 next_ = 1;
    PredictionStats stats_;
};

// Abweichung zweier Zustände: Lage in Metern.
[[nodiscard]] f64 position_error(const PredictedState& a, const PredictedState& b) noexcept;

} // namespace ngine::replication
