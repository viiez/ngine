module;

#include <optional>

// Brücke zwischen Plattformschicht und Editor-UI: Tasten, Maustasten und Mauszeiger. Beide Seiten
// haben eigene Typen, damit ngine.editor_ui ohne Fenster baut und testet.
export module editor.input;

import ngine.platform;
import ngine.editor_ui;

namespace editor {

namespace ui = ngine::editor_ui;
namespace platform = ngine::platform;

export [[nodiscard]] std::optional<ui::Key> to_ui(platform::Key key) {
    using K = platform::Key;
    switch (key) {
    case K::Tab: return ui::Key::Tab;
    case K::Left: return ui::Key::Left;
    case K::Right: return ui::Key::Right;
    case K::Up: return ui::Key::Up;
    case K::Down: return ui::Key::Down;
    case K::PageUp: return ui::Key::PageUp;
    case K::PageDown: return ui::Key::PageDown;
    case K::Home: return ui::Key::Home;
    case K::End: return ui::Key::End;
    case K::Insert: return ui::Key::Insert;
    case K::Delete: return ui::Key::Delete;
    case K::Backspace: return ui::Key::Backspace;
    case K::Space: return ui::Key::Space;
    case K::Enter: return ui::Key::Enter;
    case K::Escape: return ui::Key::Escape;
    case K::Ctrl: return ui::Key::Ctrl;
    case K::Shift: return ui::Key::Shift;
    case K::Alt: return ui::Key::Alt;
    case K::Super: return ui::Key::Super;
    case K::A: return ui::Key::A;
    case K::C: return ui::Key::C;
    case K::D: return ui::Key::D;
    case K::N: return ui::Key::N;
    case K::O: return ui::Key::O;
    case K::S: return ui::Key::S;
    case K::V: return ui::Key::V;
    case K::X: return ui::Key::X;
    case K::Y: return ui::Key::Y;
    case K::Z: return ui::Key::Z;
    case K::F2: return ui::Key::F2;
    default: return std::nullopt;
    }
}

export [[nodiscard]] std::optional<ui::MouseButton> to_ui(platform::MouseButton button) {
    switch (button) {
    case platform::MouseButton::Left: return ui::MouseButton::Left;
    case platform::MouseButton::Right: return ui::MouseButton::Right;
    case platform::MouseButton::Middle: return ui::MouseButton::Middle;
    default: return std::nullopt;
    }
}

export [[nodiscard]] platform::Cursor to_platform(ui::Cursor cursor) {
    switch (cursor) {
    case ui::Cursor::TextInput: return platform::Cursor::TextInput;
    case ui::Cursor::ResizeAll: return platform::Cursor::ResizeAll;
    case ui::Cursor::ResizeNS: return platform::Cursor::ResizeNS;
    case ui::Cursor::ResizeEW: return platform::Cursor::ResizeEW;
    case ui::Cursor::ResizeNESW: return platform::Cursor::ResizeNESW;
    case ui::Cursor::ResizeNWSE: return platform::Cursor::ResizeNWSE;
    case ui::Cursor::Hand: return platform::Cursor::Hand;
    case ui::Cursor::NotAllowed: return platform::Cursor::NotAllowed;
    case ui::Cursor::Hidden: return platform::Cursor::Hidden;
    default: return platform::Cursor::Arrow;
    }
}

} // namespace editor
