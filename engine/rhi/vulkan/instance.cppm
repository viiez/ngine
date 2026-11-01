module;

#include "vk.hpp"

#include <atomic>
#include <expected>
#include <string>

// Vulkan-Instanz mit Debug-Schicht und optionaler Surface.
module ngine.rhi.vulkan:instance;

import ngine.core;
import ngine.rhi;

namespace ngine::rhi::vulkan {

// Besitzt VkInstance, den Debug-Messenger und die Surface. Muss das letzte Vulkan-Objekt
// sein, das zerstört wird.
class Instance {
public:
    Instance() = default;
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    // Erzeugt die Instanz. Fehler der Validation Layers zählt validation_errors; der Zähler
    // muss die Instanz überleben.
    [[nodiscard]] std::expected<void, std::string> init(const DeviceDesc& desc,
                                                        std::atomic<u32>& validation_errors);
    // Erzeugt die Surface über das Fenster, das die Plattformschicht beschreibt.
    [[nodiscard]] std::expected<void, std::string> create_surface(const SurfaceDesc& surface);

    [[nodiscard]] VkInstance handle() const {
        return instance_;
    }
    // VK_NULL_HANDLE ohne Fenster.
    [[nodiscard]] VkSurfaceKHR surface() const {
        return surface_;
    }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
};

} // namespace ngine::rhi::vulkan
