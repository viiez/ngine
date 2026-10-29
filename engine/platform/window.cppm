module;

#include <expected>
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

enum class Key : u8 { Unknown, Escape };

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
} // namespace event

using Event = std::variant<event::Quit, event::Resized, event::KeyPressed>;

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
