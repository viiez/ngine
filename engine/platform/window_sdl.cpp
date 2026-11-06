module;

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module ngine.platform;

import ngine.core;

namespace ngine::platform {

struct Window::Impl {
    SDL_Window* window = nullptr;
    // Systemzeiger nach Cursor, erst bei Bedarf erzeugt.
    std::array<SDL_Cursor*, 10> cursors{};
    Cursor cursor = Cursor::Arrow;
    std::function<void(Extent)> redraw;
    bool watching = false;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (watching) {
            SDL_RemoveEventWatch(&Impl::watch, this);
        }
        for (SDL_Cursor* c : cursors) {
            if (c != nullptr) {
                SDL_DestroyCursor(c);
            }
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    // Läuft beim Eintreffen jedes Ereignisses, auch innerhalb der modalen Schleife von Windows.
    // SDL meldet dort Expose-Ereignisse mit data1 = 1 für das Zeichnen während des Ziehens.
    static bool SDLCALL watch(void* user, SDL_Event* event) {
        auto* impl = static_cast<Impl*>(user);
        if (event->type == SDL_EVENT_WINDOW_EXPOSED && event->window.data1 == 1 &&
            event->window.windowID == SDL_GetWindowID(impl->window) && impl->redraw) {
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(impl->window, &width, &height);
            impl->redraw({static_cast<u32>(width), static_cast<u32>(height)});
        }
        return true;
    }
};

std::expected<Window, std::string> Window::create(const WindowDesc& desc) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return std::unexpected(
            std::format("SDL-Video lässt sich nicht starten: {}", SDL_GetError()));
    }

    auto impl = std::make_unique<Impl>();

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    impl->window = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width),
                                    static_cast<int>(desc.height), flags);
    if (impl->window == nullptr) {
        return std::unexpected(
            std::format("Fenster lässt sich nicht erzeugen: {}", SDL_GetError()));
    }

    log::info("Fenster geöffnet: {}x{}, Video-Treiber {}", desc.width, desc.height,
              SDL_GetCurrentVideoDriver());
    return Window{std::move(impl)};
}

Window::Window(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;
Window::~Window() = default;

namespace {

Key to_key(SDL_Scancode key) {
    // Buchstaben, Ziffern und F-Tasten liegen in SDL und in Key jeweils am Stück.
    if (key >= SDL_SCANCODE_A && key <= SDL_SCANCODE_Z) {
        return static_cast<Key>(static_cast<int>(Key::A) +
                                (static_cast<int>(key) - static_cast<int>(SDL_SCANCODE_A)));
    }
    if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_9) {
        return static_cast<Key>(static_cast<int>(Key::Num1) +
                                (static_cast<int>(key) - static_cast<int>(SDL_SCANCODE_1)));
    }
    if (key >= SDL_SCANCODE_F1 && key <= SDL_SCANCODE_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) +
                                (static_cast<int>(key) - static_cast<int>(SDL_SCANCODE_F1)));
    }
    switch (key) {
    case SDL_SCANCODE_0: return Key::Num0;
    case SDL_SCANCODE_ESCAPE: return Key::Escape;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return Key::Enter;
    case SDL_SCANCODE_TAB: return Key::Tab;
    case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
    case SDL_SCANCODE_INSERT: return Key::Insert;
    case SDL_SCANCODE_DELETE: return Key::Delete;
    case SDL_SCANCODE_HOME: return Key::Home;
    case SDL_SCANCODE_END: return Key::End;
    case SDL_SCANCODE_PAGEUP: return Key::PageUp;
    case SDL_SCANCODE_PAGEDOWN: return Key::PageDown;
    case SDL_SCANCODE_LEFT: return Key::Left;
    case SDL_SCANCODE_RIGHT: return Key::Right;
    case SDL_SCANCODE_UP: return Key::Up;
    case SDL_SCANCODE_DOWN: return Key::Down;
    case SDL_SCANCODE_SPACE: return Key::Space;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return Key::Shift;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL: return Key::Ctrl;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT: return Key::Alt;
    case SDL_SCANCODE_LGUI:
    case SDL_SCANCODE_RGUI: return Key::Super;
    default: return Key::Unknown;
    }
}

SDL_SystemCursor to_system_cursor(Cursor cursor) {
    switch (cursor) {
    case Cursor::TextInput: return SDL_SYSTEM_CURSOR_TEXT;
    case Cursor::ResizeAll: return SDL_SYSTEM_CURSOR_MOVE;
    case Cursor::ResizeNS: return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case Cursor::ResizeEW: return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case Cursor::ResizeNESW: return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    case Cursor::ResizeNWSE: return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    case Cursor::Hand: return SDL_SYSTEM_CURSOR_POINTER;
    case Cursor::NotAllowed: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    default: return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

MouseButton to_button(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return MouseButton::Left;
    case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
    case SDL_BUTTON_RIGHT: return MouseButton::Right;
    default: return MouseButton::Other;
    }
}

} // namespace

std::vector<Event> Window::poll_events() {
    std::vector<Event> events;
    // SDL meldet Mauspositionen in Fensterkoordinaten; bei hoher Pixeldichte sind Pixel kleiner.
    const float density = SDL_GetWindowPixelDensity(impl_->window);
    SDL_Event sdl_event;
    while (SDL_PollEvent(&sdl_event)) {
        switch (sdl_event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: events.emplace_back(event::Quit{}); break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            events.emplace_back(event::Resized{{static_cast<u32>(sdl_event.window.data1),
                                                static_cast<u32>(sdl_event.window.data2)}});
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!sdl_event.key.repeat) {
                events.emplace_back(event::KeyPressed{to_key(sdl_event.key.scancode)});
            }
            break;
        case SDL_EVENT_KEY_UP:
            events.emplace_back(event::KeyReleased{to_key(sdl_event.key.scancode)});
            break;
        case SDL_EVENT_MOUSE_MOTION:
            events.emplace_back(event::MouseMoved{
                sdl_event.motion.x * density, sdl_event.motion.y * density,
                sdl_event.motion.xrel * density, sdl_event.motion.yrel * density});
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            events.emplace_back(event::MouseButtonPressed{to_button(sdl_event.button.button),
                                                          sdl_event.button.x * density,
                                                          sdl_event.button.y * density});
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            events.emplace_back(event::MouseButtonReleased{to_button(sdl_event.button.button),
                                                           sdl_event.button.x * density,
                                                           sdl_event.button.y * density});
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            const float sign = sdl_event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            events.emplace_back(
                event::MouseWheel{sdl_event.wheel.y * sign, sdl_event.wheel.x * sign});
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            if (sdl_event.text.text != nullptr) {
                events.emplace_back(event::TextInput{sdl_event.text.text});
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: events.emplace_back(event::FocusChanged{true}); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: events.emplace_back(event::FocusChanged{false}); break;
        default: break;
        }
    }
    return events;
}

Extent Window::framebuffer_size() const {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(impl_->window, &width, &height);
    return {static_cast<u32>(width), static_cast<u32>(height)};
}

bool Window::minimized() const {
    return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_MINIMIZED) != 0;
}

void Window::set_relative_mouse(bool enabled) {
    if (!SDL_SetWindowRelativeMouseMode(impl_->window, enabled)) {
        log::warn("Relativer Mausmodus nicht verfügbar: {}", SDL_GetError());
    }
}

void Window::set_cursor(Cursor cursor) {
    if (cursor == impl_->cursor) {
        return;
    }
    impl_->cursor = cursor;
    if (cursor == Cursor::Hidden) {
        SDL_HideCursor();
        return;
    }
    SDL_Cursor*& system = impl_->cursors[static_cast<usize>(cursor)];
    if (system == nullptr) {
        system = SDL_CreateSystemCursor(to_system_cursor(cursor));
    }
    if (system != nullptr) {
        SDL_SetCursor(system);
    }
    SDL_ShowCursor();
}

void Window::set_text_input(bool enabled) {
    if (enabled == SDL_TextInputActive(impl_->window)) {
        return;
    }
    const bool done =
        enabled ? SDL_StartTextInput(impl_->window) : SDL_StopTextInput(impl_->window);
    if (!done) {
        log::warn("Texteingabe lässt sich nicht umschalten: {}", SDL_GetError());
    }
}

void Window::set_title(const std::string& title) {
    if (!SDL_SetWindowTitle(impl_->window, title.c_str())) {
        log::warn("Fenstertitel lässt sich nicht setzen: {}", SDL_GetError());
    }
}

std::string Window::clipboard_text() const {
    char* text = SDL_GetClipboardText();
    if (text == nullptr) {
        return {};
    }
    std::string result{text};
    SDL_free(text);
    return result;
}

void Window::set_clipboard_text(const std::string& text) {
    if (!SDL_SetClipboardText(text.c_str())) {
        log::warn("Zwischenablage lässt sich nicht setzen: {}", SDL_GetError());
    }
}

f32 Window::display_scale() const {
    const float scale = SDL_GetWindowDisplayScale(impl_->window);
    return scale > 0.0f ? scale : 1.0f;
}

void Window::set_redraw_callback(std::function<void(Extent framebuffer)> callback) {
    impl_->redraw = std::move(callback);
    if (!impl_->watching && impl_->redraw) {
        impl_->watching = SDL_AddEventWatch(&Impl::watch, impl_.get());
    }
}

std::vector<const char*> Window::vulkan_instance_extensions() const {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (names == nullptr) {
        return {};
    }
    return {names, names + count};
}

u64 Window::create_vulkan_surface(void* vk_instance) const {
    static_assert(sizeof(VkSurfaceKHR) == sizeof(u64), "Nur 64-Bit-Plattformen werden unterstützt");
    VkSurfaceKHR surface{};
    if (!SDL_Vulkan_CreateSurface(impl_->window, static_cast<VkInstance>(vk_instance), nullptr,
                                  &surface)) {
        log::error("Vulkan-Surface lässt sich nicht erzeugen: {}", SDL_GetError());
        return 0;
    }
    return std::bit_cast<u64>(surface);
}

} // namespace ngine::platform
