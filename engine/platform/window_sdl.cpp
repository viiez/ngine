module;

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <bit>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module ngine.platform;

namespace ngine::platform {

struct Window::Impl {
    SDL_Window* window = nullptr;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
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

std::vector<Event> Window::poll_events() {
    std::vector<Event> events;
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
            events.emplace_back(
                event::KeyPressed{sdl_event.key.key == SDLK_ESCAPE ? Key::Escape : Key::Unknown});
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
