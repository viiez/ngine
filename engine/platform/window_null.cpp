module;

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Plattform ohne SDL3 (NGINE_WITH_WINDOW=OFF): Es gibt kein Fenster, aber die Typen der Ereignisse,
// damit ngine.input, der Server und die Tests ohne Fenster bauen (R4.1).
module ngine.platform;

import ngine.core;

namespace ngine::platform {

struct Window::Impl {};

std::expected<Window, std::string> Window::create(const WindowDesc& /*desc*/) {
    return std::unexpected(std::string{"Ohne Fenster gebaut (NGINE_WITH_WINDOW=OFF)"});
}

Window::Window(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;
Window::~Window() = default;

std::vector<Event> Window::poll_events() {
    return {};
}
Extent Window::framebuffer_size() const {
    return {};
}
bool Window::minimized() const {
    return false;
}
void Window::set_relative_mouse(bool /*enabled*/) {}
void Window::set_cursor(Cursor /*cursor*/) {}
void Window::set_text_input(bool /*enabled*/) {}
void Window::set_title(const std::string& /*title*/) {}
std::string Window::clipboard_text() const {
    return {};
}
void Window::set_clipboard_text(const std::string& /*text*/) {}
f32 Window::display_scale() const {
    return 1.0f;
}
void Window::set_redraw_callback(std::function<void(Extent framebuffer)> /*callback*/) {}
std::vector<const char*> Window::vulkan_instance_extensions() const {
    return {};
}
u64 Window::create_vulkan_surface(void* /*vk_instance*/) const {
    return 0;
}

} // namespace ngine::platform
