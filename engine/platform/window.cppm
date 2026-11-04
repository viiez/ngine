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
// französischen Tastatur dort, wo die freie Kamera sie erwartet. Die Abbildung auf Aktionen kommt
// mit PLAT-02 in R2.
enum class Key : u8 { Unknown, Escape, Space, W, A, S, D, Q, E, Shift, F1, F2 };
enum class MouseButton : u8 { Left, Middle, Right, Other };

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
// Positiv: vom Nutzer weg gedreht.
struct MouseWheel {
    f32 delta;
};
} // namespace event

using Event = std::variant<event::Quit, event::Resized, event::KeyPressed, event::KeyReleased,
                           event::MouseMoved, event::MouseButtonPressed, event::MouseButtonReleased,
                           event::MouseWheel>;

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
