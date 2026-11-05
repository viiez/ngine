module;

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Schmale Schicht im Stil von Dear ImGui (ADR 0029): Fenster, Menüs, Text, Bilder, einfache
// Eingaben und Andocken. Bewusst keine eigene UI-Bibliothek; was der Editor braucht, kommt dazu.
// Alle Funktionen gelten für den Kontext, der gerade einen Frame zeichnet (zwischen
// Context::begin_frame() und end_frame()).
export module ngine.editor_ui:widgets;

import ngine.core;
import :draw;

export namespace ngine::editor_ui {

struct Size {
    f32 width = 0.0f;
    f32 height = 0.0f;
};

struct Point {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

// Farbe für Text, linear 0 bis 1 wie in ImGui (sRGB-kodiert).
struct UiColor {
    f32 r = 1.0f;
    f32 g = 1.0f;
    f32 b = 1.0f;
    f32 a = 1.0f;
};

enum class WindowFlags : u8 {
    None = 0,
    // Inhalt bis an den Rand, z. B. für ein Bild, das das Fenster füllt.
    NoPadding = 1,
    NoScrollbar = 2,
    // Mausrad scrollt nicht (die Anwendung benutzt es selbst).
    NoScrollWithMouse = 4,
};

[[nodiscard]] constexpr WindowFlags operator|(WindowFlags a, WindowFlags b) noexcept {
    return static_cast<WindowFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool has(WindowFlags set, WindowFlags flag) noexcept {
    return (static_cast<u8>(set) & static_cast<u8>(flag)) != 0;
}

// Anordnung der Fenster beim ersten Start (oder nach reset_layout()): ein Bereich in der Mitte,
// links, rechts und unten je ein Streifen mit dem angegebenen Anteil. Leere Bereiche entfallen.
struct DockLayout {
    std::vector<std::string> center;
    std::vector<std::string> left;
    std::vector<std::string> right;
    std::vector<std::string> bottom;
    f32 left_ratio = 0.2f;
    f32 right_ratio = 0.25f;
    f32 bottom_ratio = 0.28f;
};

// Andockfläche über das ganze Fenster unter der Menüleiste. Hat sie noch keine Anordnung (erster
// Start ohne gespeicherte Lage), ordnet sie die Fenster nach layout an. Vor den Fenstern aufrufen.
void dockspace(const DockLayout& layout);
// Ordnet die Fenster beim nächsten dockspace() wieder nach dem Layout an.
void reset_layout();

// Fenster; end_window() immer aufrufen, auch wenn begin_window() false liefert (dann ist es
// eingeklappt oder verdeckt). Mit open bekommt es ein Schließfeld.
bool begin_window(std::string_view title, bool* open = nullptr,
                  WindowFlags flags = WindowFlags::None);
void end_window();

// Bereich mit eigenem Scrollen innerhalb eines Fensters; end_child() immer aufrufen.
bool begin_child(std::string_view id, Size size = {}, bool border = false);
void end_child();

// Menüleiste am oberen Rand des Hauptfensters; end_main_menu_bar() nur, wenn begin true lieferte.
bool begin_main_menu_bar();
void end_main_menu_bar();
// end_menu() nur, wenn begin_menu() true lieferte.
bool begin_menu(std::string_view label);
void end_menu();
// Liefert true, wenn angeklickt. Mit selected wird ein Haken gezeigt und umgeschaltet.
bool menu_item(std::string_view label, std::string_view shortcut = {}, bool* selected = nullptr,
               bool enabled = true);

void text(std::string_view text);
void text_colored(UiColor color, std::string_view text);
void text_disabled(std::string_view text);
// Text, der am Fensterrand umbricht.
void text_wrapped(std::string_view text);
void separator();
// Nächstes Element in dieselbe Zeile.
void same_line(f32 spacing = -1.0f);

bool button(std::string_view label);
bool checkbox(std::string_view label, bool& value);
// Eingabefeld für eine Zeile Text; liefert true, wenn sich der Text geändert hat.
bool input_text(std::string_view label, std::string& value);
// Breite des nächsten Elements in Pixeln; negativ: bis so weit vor den rechten Rand.
void set_next_item_width(f32 width);

// Zeigt eine Textur (TextureId aus dem Pass) in der angegebenen Größe; uv von oben links.
void image(TextureId texture, Size size, Point uv0 = {0.0f, 0.0f}, Point uv1 = {1.0f, 1.0f});

// Freier Platz im aktuellen Fenster ab der Schreibposition.
[[nodiscard]] Size content_region_available();
// Linke obere Ecke des letzten Elements in Pixeln der Zeichenfläche.
[[nodiscard]] Point item_min();
[[nodiscard]] bool is_item_hovered();
// Ob die Maus über dem aktuellen Fenster ist (und nicht von einem Popup verdeckt).
[[nodiscard]] bool is_window_hovered();
[[nodiscard]] bool is_window_focused();
// Gibt dem aktuellen Fenster den Fokus.
void focus_window();

// Scrollposition des aktuellen Fensters bzw. Bereichs.
[[nodiscard]] f32 scroll_y();
[[nodiscard]] f32 scroll_max_y();
// Scrollt so, dass die aktuelle Schreibposition sichtbar ist (0 oben, 1 unten).
void scroll_here(f32 ratio = 1.0f);

// Zeichnet von count gleich hohen Zeilen nur die sichtbaren: row bekommt jeweils einen Bereich
// [first, last) und gibt diese Zeilen aus.
void clipped_rows(usize count, const std::function<void(usize first, usize last)>& rows);

} // namespace ngine::editor_ui
