module;

#include <array>
#include <bitset>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module ngine.input:actions;

import ngine.core;
import ngine.platform;
import ngine.reflect;

export namespace ngine::input {

template <typename T>
using Result = std::expected<T, std::string>;

// Art einer Aktion.
enum class ActionKind : u8 {
    // Taste: Wert 0 bis 1, gedrückt ab der Hälfte (z. B. jump).
    Button,
    // Eine Achse, Summe aller Quellen (z. B. zoom).
    Axis,
    // Zwei Achsen x und y (z. B. move aus WASD und linkem Stick).
    Axis2,
};

// Welche Achse einer Axis2-Aktion eine Quelle speist.
enum class Component : u8 { X, Y };

// Eine Quelle einer Aktion. source ist ein Name wie "Key.W", "Mouse.Left", "Mouse.X",
// "Mouse.Wheel", "Gamepad.South" oder "Gamepad.LeftX" (Liste in source_names()).
struct Binding {
    std::string source;
    // Bei Axis2: welche Achse.
    Component component = Component::X;
    // Faktor auf den Wert der Quelle, z. B. -1 für S bei move.y.
    f32 scale = 1.0f;
    // Der Wert der Quelle ist eine Rate pro Sekunde und wird mit der Bildzeit multipliziert. So
    // liefert ein Stick für look dieselbe Art Wert wie die Maus (Bewegung pro Bild).
    bool per_second = false;
};

// Eine benannte Aktion mit ihren Quellen. Mehrere Quellen pro Aktion addieren sich (Achsen) bzw.
// die stärkste zählt (Tasten).
struct Action {
    std::string name;
    ActionKind kind = ActionKind::Button;
    // Tote Zone für Sticks und Trigger (0 bis 1): Werte darunter zählen als 0, darüber wird neu
    // auf 0 bis 1 skaliert.
    f32 dead_zone = 0.2f;
    // Faktor auf den Wert der ganzen Aktion.
    f32 sensitivity = 1.0f;
    // Längste erlaubte Länge des Werts (Axis2) bzw. größter Betrag (Axis); 0: unbegrenzt. 1 für
    // move hält die Diagonale von WASD so schnell wie eine Richtung.
    f32 limit = 0.0f;
    std::vector<Binding> bindings;
};

// Registriert ActionKind, Component, Binding und Action in der Reflection (für die Datei).
void register_types(reflect::TypeRegistry& types);

// Belegung: alle Aktionen mit ihren Quellen. Kommt aus einer JSON-Datei
// (assets/eingabe/standard.input):
//
//   { "actions": [ { "name": "jump", "kind": "Button",
//                    "bindings": [ { "source": "Key.Space" }, { "source": "Gamepad.South" } ] } ] }
//
// Die Felder einer Aktion und einer Quelle liest die Reflection; unbekannte Felder werden
// übersprungen, unbekannte Quellen sind ein Fehler.
class ActionMap {
public:
    ActionMap() = default;

    [[nodiscard]] static Result<ActionMap> parse(std::string_view text);
    [[nodiscard]] static Result<ActionMap> load_file(std::string_view path);
    // Die eingebaute Belegung (gleich assets/eingabe/standard.input), wenn keine Datei da ist.
    [[nodiscard]] static ActionMap standard();

    // Fügt eine Aktion hinzu; eine gleichnamige wird ersetzt. Unbekannte Quellen sind ein Fehler.
    Result<void> add(Action action);

    [[nodiscard]] const std::vector<Action>& actions() const noexcept {
        return actions_;
    }
    // Index der Aktion oder nullopt.
    [[nodiscard]] std::optional<usize> find(std::string_view name) const noexcept;

    // Vorverarbeitete Quelle: Art des Geräts und Nummer der Taste oder Achse.
    enum class Device : u8 { Key, MouseButton, MouseAxis, GamepadButton, GamepadAxis };
    struct Source {
        Device device;
        u8 code;
    };
    // Quellen der Aktion index in der Reihenfolge ihrer Bindings.
    [[nodiscard]] const std::vector<Source>& sources(usize index) const noexcept {
        return sources_[index];
    }

private:
    std::vector<Action> actions_;
    std::vector<std::vector<Source>> sources_;
};

// Alle gültigen Namen für Binding::source.
[[nodiscard]] std::vector<std::string> source_names();

// Wert und Flanken einer Aktion.
struct ActionValue {
    // Button und Axis: x; Axis2: x und y.
    f32 x = 0.0f;
    f32 y = 0.0f;
    // Gehalten (Button: Wert ab 0,5; Achsen: nicht 0).
    bool down = false;
    // In diesem Bild gerade gedrückt bzw. losgelassen.
    bool pressed = false;
    bool released = false;
};

// Zustand aller Aktionen einer Belegung. Pro Bild:
//
//   state.begin_frame();
//   for (const auto& event : window.poll_events()) state.handle(event);
//   state.end_frame(dt);
//
// Danach liefern value(), down(), pressed() und released() den Stand des Bildes. Für die feste
// Simulation gibt es fixed(): Dort bleiben Flanken stehen, bis ein fester Schritt sie gesehen hat
// (end_fixed_step()). Ein kurzer Tastendruck geht so bei niedriger Bildrate nicht verloren und
// zählt bei hoher nicht doppelt. Läuft ohne Fenster: Tests und der Server füttern Ereignisse
// direkt.
class ActionState {
public:
    ActionState() = default;
    explicit ActionState(ActionMap map);

    // Neue Belegung, z. B. nach Änderung der Datei. Gehaltene Tasten bleiben gehalten.
    void set_map(ActionMap map);
    [[nodiscard]] const ActionMap& map() const noexcept {
        return map_;
    }

    void begin_frame();
    void handle(const platform::Event& event);
    void end_frame(f32 dt);
    // Vergisst gehaltene Tasten und Bewegungen, z. B. wenn das Fenster den Fokus verliert oder die
    // UI die Eingabe übernimmt.
    void release_all();

    // Stand im Bild. Unbekannte Namen liefern einen leeren Wert.
    [[nodiscard]] const ActionValue& value(std::string_view action) const noexcept;
    [[nodiscard]] bool down(std::string_view action) const noexcept {
        return value(action).down;
    }
    [[nodiscard]] bool pressed(std::string_view action) const noexcept {
        return value(action).pressed;
    }
    [[nodiscard]] bool released(std::string_view action) const noexcept {
        return value(action).released;
    }

    // Stand für die feste Simulation: Werte wie im Bild, Flanken gesammelt seit dem letzten
    // end_fixed_step(). down ist auch wahr, wenn eine Taste seitdem kurz gedrückt war.
    [[nodiscard]] const ActionValue& fixed(std::string_view action) const noexcept;
    // Nach jedem festen Schritt (Simulation::set_after_fixed_step()).
    void end_fixed_step();

    // Zahl der angesteckten Gamepads.
    [[nodiscard]] usize gamepad_count() const noexcept {
        return gamepads_.size();
    }

private:
    static constexpr usize key_count = static_cast<usize>(platform::Key::Count);
    static constexpr usize mouse_button_count = 4;
    // Mausachsen: X, Y, Wheel, WheelX.
    static constexpr usize mouse_axis_count = 4;
    static constexpr usize pad_button_count = static_cast<usize>(platform::GamepadButton::Count);
    static constexpr usize pad_axis_count = static_cast<usize>(platform::GamepadAxis::Count);

    struct Gamepad {
        u32 id = 0;
        std::bitset<pad_button_count> buttons;
        std::array<f32, pad_axis_count> axes{};
    };

    [[nodiscard]] f32 source_value(const ActionMap::Source& source,
                                   const Action& action) const noexcept;
    [[nodiscard]] bool source_pressed(const ActionMap::Source& source) const noexcept;
    Gamepad* gamepad(u32 id) noexcept;

    ActionMap map_;
    std::vector<ActionValue> values_;
    std::vector<ActionValue> fixed_;

    std::bitset<key_count> keys_;
    std::bitset<key_count> keys_pressed_;
    std::bitset<mouse_button_count> mouse_;
    std::bitset<mouse_button_count> mouse_pressed_;
    std::array<f32, mouse_axis_count> mouse_axes_{};
    std::vector<Gamepad> gamepads_;
    std::bitset<pad_button_count> pad_pressed_;
};

} // namespace ngine::input
