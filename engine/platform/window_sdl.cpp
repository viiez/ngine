module;

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

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
    std::function<void(Extent)> redraw;
    bool watching = false;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (watching) {
            SDL_RemoveEventWatch(&Impl::watch, this);
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
    switch (key) {
    case SDL_SCANCODE_ESCAPE: return Key::Escape;
    case SDL_SCANCODE_SPACE: return Key::Space;
    case SDL_SCANCODE_W: return Key::W;
    case SDL_SCANCODE_A: return Key::A;
    case SDL_SCANCODE_S: return Key::S;
    case SDL_SCANCODE_D: return Key::D;
    case SDL_SCANCODE_Q: return Key::Q;
    case SDL_SCANCODE_E: return Key::E;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return Key::Shift;
    default: return Key::Unknown;
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
        case SDL_EVENT_MOUSE_WHEEL:
            events.emplace_back(event::MouseWheel{sdl_event.wheel.y});
            break;
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
