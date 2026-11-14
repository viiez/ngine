module;

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

export module ngine.platform:window;

import ngine.core;

export namespace ngine::platform {

struct Extent {
    u32 width = 0;
    u32 height = 0;
};

struct WindowDesc {
    std::string title = "NGine";
    u32 width = 1280;
    u32 height = 720;
    bool resizable = true;
};

// Tasten nach ihrer Lage auf einer US-Tastatur (Scancodes): W, A, S, D liegen auch auf einer
// französischen Tastatur dort, wo die freie Kamera sie erwartet. Für Text gibt es TextInput, das
// die Belegung berücksichtigt. Linke und rechte Umschalttasten melden dieselbe Taste. Benannte
// Aktionen bildet ngine.input darauf ab (PLAT-02). Neue Tasten kommen ans Ende, damit die Werte
// der übrigen bleiben.
enum class Key : u8 {
    Unknown,
    // clang-format off
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    // clang-format on
    Escape,
    Enter,
    Tab,
    Backspace,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    Left,
    Right,
    Up,
    Down,
    Space,
    Shift,
    Ctrl,
    Alt,
    Super,
    // Seit R4.1, auf einer deutschen Tastatur z. B. Ü, +, Komma, Punkt, ß und Akut.
    LeftBracket,
    RightBracket,
    Comma,
    Period,
    Minus,
    Equals,
    Count,
};
enum class MouseButton : u8 { Left, Middle, Right, Other };

// Tasten eines Gamepads in der einheitlichen Belegung von SDL nach dem Xbox-Schema: South ist A
// (PlayStation: Kreuz), East ist B (Kreis), West ist X (Quadrat), North ist Y (Dreieck).
enum class GamepadButton : u8 {
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Count,
};

// Achsen eines Gamepads. Sticks liefern -1 bis 1, nach rechts und nach oben positiv; Trigger
// liefern 0 bis 1.
enum class GamepadAxis : u8 {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count,
};

// Form des Mauszeigers.
enum class Cursor : u8 {
    Arrow,
    TextInput,
    ResizeAll,
    ResizeNS,
    ResizeEW,
    ResizeNESW,
    ResizeNWSE,
    Hand,
    NotAllowed,
    Hidden,
};

namespace event {
// Der Nutzer will das Fenster schließen.
struct Quit {};
// Die Größe der Zeichenfläche in Pixeln hat sich geändert.
struct Resized {
    Extent framebuffer;
};
struct KeyPressed {
    Key key;
};
struct KeyReleased {
    Key key;
};
// Mauspositionen in Pixeln der Zeichenfläche, Ursprung oben links. dx und dy sind die Bewegung seit
// dem letzten Ereignis, auch im relativen Modus, in dem die Position stehen bleibt.
struct MouseMoved {
    f32 x;
    f32 y;
    f32 dx = 0.0f;
    f32 dy = 0.0f;
};
struct MouseButtonPressed {
    MouseButton button;
    f32 x;
    f32 y;
};
struct MouseButtonReleased {
    MouseButton button;
    f32 x;
    f32 y;
};
// Positiv: vom Nutzer weg gedreht bzw. nach rechts geschoben.
struct MouseWheel {
    f32 delta;
    f32 delta_x = 0.0f;
};
// Eingegebener Text (UTF-8), nach Tastaturbelegung und Eingabemethode. Kommt nur, solange die
// Texteingabe eingeschaltet ist (Window::set_text_input()).
struct TextInput {
    std::string text;
};
// Das Fenster hat den Tastaturfokus bekommen oder verloren.
struct FocusChanged {
    bool focused;
};
// Ein Gamepad ist angesteckt worden (oder war beim Start schon da). gamepad ist eine Kennung, die
// für dieses Gerät gilt, bis es abgezogen wird.
struct GamepadAdded {
    u32 gamepad;
    std::string name;
};
struct GamepadRemoved {
    u32 gamepad;
};
struct GamepadButtonPressed {
    u32 gamepad;
    GamepadButton button;
};
struct GamepadButtonReleased {
    u32 gamepad;
    GamepadButton button;
};
struct GamepadAxisMoved {
    u32 gamepad;
    GamepadAxis axis;
    f32 value;
};
} // namespace event

using Event = std::variant<event::Quit, event::Resized, event::KeyPressed, event::KeyReleased,
                           event::MouseMoved, event::MouseButtonPressed, event::MouseButtonReleased,
                           event::MouseWheel, event::TextInput, event::FocusChanged,
                           event::GamepadAdded, event::GamepadRemoved, event::GamepadButtonPressed,
                           event::GamepadButtonReleased, event::GamepadAxisMoved>;

// Ein Fenster mit Eingabe. Ohne SDL3 (NGINE_WITH_WINDOW=OFF) liefert create() einen Fehler; die
// Typen der Ereignisse gibt es trotzdem, damit ngine.input und Tests ohne Fenster bauen.
class Window {
public:
    static std::expected<Window, std::string> create(const WindowDesc& desc);

    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    // Holt alle seit dem letzten Aufruf angefallenen Ereignisse.
    [[nodiscard]] std::vector<Event> poll_events();

    [[nodiscard]] Extent framebuffer_size() const;
    [[nodiscard]] bool minimized() const;

    // Relativer Mausmodus: Der Zeiger verschwindet und bleibt im Fenster, MouseMoved meldet nur
    // noch Bewegungen (für eine Kamera, die man mit der Maus dreht).
    void set_relative_mouse(bool enabled);

    // Form des Mauszeigers über dem Fenster; Hidden versteckt ihn.
    void set_cursor(Cursor cursor);

    // Schaltet TextInput-Ereignisse ein oder aus. Auf manchen Plattformen blendet das eine
    // Bildschirmtastatur oder eine Eingabemethode ein, deshalb nur, solange ein Textfeld aktiv ist.
    void set_text_input(bool enabled);

    // Titelzeile des Fensters, z. B. mit dem Namen der offenen Datei.
    void set_title(const std::string& title);

    // Inhalt der Zwischenablage als UTF-8; leer, wenn sie keinen Text enthält.
    [[nodiscard]] std::string clipboard_text() const;
    void set_clipboard_text(const std::string& text);

    // Skalierung des Bildschirms, auf dem das Fenster liegt (1 bei 100 %, 1.5 bei 150 %). Gilt für
    // Größen in Pixeln der Zeichenfläche, z. B. Schrift und Abstände einer UI.
    [[nodiscard]] f32 display_scale() const;

    // Wird aufgerufen, wenn das Fenster neu gezeichnet werden muss, während poll_events() nicht
    // zurückkehren kann: Unter Windows hält das System die Anwendung beim Ziehen am Fensterrand in
    // einer eigenen Schleife fest (TD-19). Der Rückruf bekommt die aktuelle Größe der Zeichenfläche
    // und sollte einen ganzen Frame zeichnen. Unter Linux kommt er nicht vor.
    void set_redraw_callback(std::function<void(Extent framebuffer)> callback);

    // Brücke zu Vulkan, ohne dass die Plattformschicht Vulkan-Typen kennt:
    // Instanz-Erweiterungen für die Surface und das Erzeugen der Surface selbst.
    // create_vulkan_surface() bekommt eine VkInstance und liefert eine VkSurfaceKHR
    // als Zahl, 0 bei Fehler.
    [[nodiscard]] std::vector<const char*> vulkan_instance_extensions() const;
    [[nodiscard]] u64 create_vulkan_surface(void* vk_instance) const;

private:
    struct Impl;
    explicit Window(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::platform
