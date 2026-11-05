module;

#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Zustand der Editor-UI: ein Kontext von Dear ImGui mit Docking (ADR 0029). Eingaben kommen in
// Engine-Typen herein, die Zeichenliste geht als UiDrawData hinaus; nur die Implementierung kennt
// ImGui.
export module ngine.editor_ui:context;

import ngine.core;
import :draw;

export namespace ngine::editor_ui {

// Tasten, die die UI versteht (Navigation, Textbearbeitung, Tastenkürzel). Umschalttasten melden
// links und rechts gemeinsam.
enum class Key : u8 {
    Tab,
    Left,
    Right,
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
    Insert,
    Delete,
    Backspace,
    Space,
    Enter,
    Escape,
    Ctrl,
    Shift,
    Alt,
    Super,
    A,
    C,
    V,
    X,
    Y,
    Z,
};

enum class MouseButton : u8 { Left, Right, Middle };

// Mauszeiger, den die UI gerade wünscht.
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

struct ContextDesc {
    // Datei, in der die UI Lage und Andocken der Fenster speichert; leer: nicht speichern.
    std::string ini_path;
    // Skalierung von Schrift und Abständen (Bildschirmskalierung, z. B. 1.5 bei 150 %).
    f32 scale = 1.0f;
    // Zwischenablage; ohne Rückrufe bleibt sie innerhalb der UI.
    std::function<std::string()> get_clipboard;
    std::function<void(const std::string&)> set_clipboard;
};

// Ein Frame der UI: begin_frame(), dann Fenster über die Funktionen aus :widgets, dann end_frame().
// Eingaben dürfen jederzeit kommen; sie wirken im nächsten begin_frame(). Ein Kontext gehört
// einem Thread. Der TextureHost muss den Kontext überleben.
class Context {
public:
    Context(TextureHost& textures, const ContextDesc& desc = {});
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Eingaben, Positionen in Pixeln der Zeichenfläche (Ursprung oben links).
    void add_mouse_position(f32 x, f32 y);
    // Die Maus hat das Fenster verlassen (oder gehört gerade jemand anderem).
    void add_mouse_left_window();
    void add_mouse_button(MouseButton button, bool down);
    // Positiv: vom Nutzer weg bzw. nach rechts.
    void add_mouse_wheel(f32 delta_y, f32 delta_x = 0.0f);
    void add_key(Key key, bool down);
    // Eingegebener Text in UTF-8.
    void add_text(std::string_view utf8);
    void add_focus(bool focused);

    // Beginnt einen Frame mit der Größe der Zeichenfläche in Pixeln und der Zeit seit dem letzten.
    void begin_frame(f32 width, f32 height, f32 dt);
    // Schließt den Frame ab: legt Texturen der UI an oder gibt sie frei und liefert die
    // Zeichenliste. Sie bleibt bis zum nächsten end_frame() gültig.
    const UiDrawData& end_frame();

    // Ob die UI Maus bzw. Tastatur gerade selbst braucht (dann gehören die Eingaben nicht der
    // Anwendung, z. B. nicht der Kamera).
    [[nodiscard]] bool wants_mouse() const;
    [[nodiscard]] bool wants_keyboard() const;
    // Ob gerade ein Textfeld aktiv ist (Texteingabe der Plattform einschalten).
    [[nodiscard]] bool wants_text_input() const;
    [[nodiscard]] Cursor cursor() const;

    // Speichert Lage und Andocken der Fenster sofort (sonst nur hin und wieder und am Ende).
    void save_layout();

    [[nodiscard]] u64 frame_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::editor_ui
