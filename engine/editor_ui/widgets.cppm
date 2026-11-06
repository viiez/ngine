module;

#include <functional>
#include <optional>
#include <span>
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
import :context;

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
// Eingabefeld für eine Zeile Text; liefert true, wenn sich der Text geändert hat. Mit
// enter_returns erst, wenn Enter gedrückt wurde (der Text ändert sich trotzdem sofort).
bool input_text(std::string_view label, std::string& value, bool enter_returns = false);

// Art der Zahlen für drag_scalars().
enum class DataType : u8 { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

// Bis zu vier Zahlen nebeneinander, mit der Maus zu ziehen (speed pro Pixel) oder per Doppelklick
// einzugeben. min und max begrenzen das Ziehen (nullptr: unbegrenzt). format wie printf, leer für
// die Voreinstellung der Art. Liefert true, wenn sich ein Wert geändert hat.
bool drag_scalars(std::string_view label, DataType type, void* data, u32 count, f32 speed,
                  const f64* min = nullptr, const f64* max = nullptr, std::string_view format = {});
// Farbe aus 3 oder 4 Gleitkommazahlen (linear, 0 bis 1) mit Farbwähler.
bool color_edit(std::string_view label, f32* color, u32 count);
// Auswahlliste; current ist der Index in items.
bool combo(std::string_view label, usize& current, std::span<const std::string> items);
// Aufklappbare Überschrift über die ganze Breite.
bool collapsing_header(std::string_view label, bool default_open = true);
bool selectable(std::string_view label, bool selected = false);
void indent(f32 width = 0.0f);
void unindent(f32 width = 0.0f);
// Leerer Platz der Größe size.
void dummy(Size size);

// --- Bäume --------------------------------------------------------------------------------------

enum class TreeFlags : u8 {
    None = 0,
    // Ohne Kinder: kein Pfeil, tree_pop() entfällt.
    Leaf = 1,
    Selected = 2,
    DefaultOpen = 4,
};

[[nodiscard]] constexpr TreeFlags operator|(TreeFlags a, TreeFlags b) noexcept {
    return static_cast<TreeFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool has(TreeFlags set, TreeFlags flag) noexcept {
    return (static_cast<u8>(set) & static_cast<u8>(flag)) != 0;
}

// Knoten mit eigener Kennung id und Beschriftung label; ein Klick auf den Pfeil klappt auf, ein
// Klick auf den Text lässt sich mit is_item_clicked() abfragen. Liefert true, wenn offen; dann
// (außer bei Leaf) tree_pop() aufrufen.
bool tree_node(u64 id, std::string_view label, TreeFlags flags = TreeFlags::None);
void tree_pop();

// --- Ziehen und Ablegen
// ---------------------------------------------------------------------------

// Direkt nach einem Element: macht es zur Quelle für payload unter dem Namen type (höchstens 32
// Zeichen); preview erscheint am Mauszeiger.
void drag_source(std::string_view type, u64 payload, std::string_view preview);
// Direkt nach einem Element: liefert payload, wenn hier etwas vom Typ type abgelegt wurde.
[[nodiscard]] std::optional<u64> drop_target(std::string_view type);

// --- Popups ------------------------------------------------------------------------------------
// end_popup() nur, wenn begin_…() true lieferte.

void open_popup(std::string_view id);
bool begin_popup(std::string_view id);
// Modales Fenster mit Titelzeile; mit open bekommt es ein Schließfeld.
bool begin_popup_modal(std::string_view title, bool* open = nullptr);
// Kontextmenü des letzten Elements (Rechtsklick).
bool begin_popup_context_item(std::string_view id);
// Kontextmenü bei Rechtsklick auf eine freie Stelle des aktuellen Fensters.
bool begin_popup_context_window(std::string_view id);
void end_popup();
void close_current_popup();

// --- Zustand und Eingabe -------------------------------------------------------------------------

[[nodiscard]] bool is_item_clicked(MouseButton button = MouseButton::Left);
// Das letzte Element wurde gerade losgelassen, nachdem es seinen Wert geändert hat (Ende des
// Ziehens, Enter in einem Feld). Gut, um zusammengefasste Änderungen abzuschließen.
[[nodiscard]] bool is_item_deactivated_after_edit();
[[nodiscard]] bool is_item_deactivated();
[[nodiscard]] bool is_any_item_active();
// Taste in diesem Frame gedrückt; mit repeat auch bei gehaltener Taste wiederholt.
[[nodiscard]] bool is_key_pressed(Key key, bool repeat = false);
[[nodiscard]] bool is_key_down(Key key);
[[nodiscard]] bool is_mouse_clicked(MouseButton button);
[[nodiscard]] Point mouse_position();
void set_keyboard_focus_here();
void set_tooltip(std::string_view text);

// Kennungen für Elemente mit gleicher Beschriftung (z. B. pro Entity oder Komponente).
void push_id(u64 id);
void push_id(std::string_view id);
void pop_id();
// Elemente dazwischen sind ausgegraut und reagieren nicht.
void begin_disabled(bool disabled = true);
void end_disabled();
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
