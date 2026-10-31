module;

#include "vk.hpp"

#include <expected>
#include <string>

// Auswahl der GPU nach der Mindestanforderung (ADR 0014) und Erzeugen des logischen Geräts.
module ngine.rhi.vulkan:physical_device;

import ngine.core;
import ngine.rhi;

namespace ngine::rhi::vulkan {

struct PhysicalDevice {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    // Queue-Familie für Grafik und, mit Surface, Präsentation.
    u32 queue_family = 0;
    Capabilities capabilities;
};

// Wählt die beste GPU, die die Mindestanforderung erfüllt; dedizierte GPUs vor integrierten.
// Mit surface muss die Queue-Familie auch präsentieren können.
[[nodiscard]] std::expected<PhysicalDevice, std::string> pick_physical_device(VkInstance instance,
                                                                              VkSurfaceKHR surface);

// Erzeugt das logische Gerät mit allen Pflicht-Features und lädt seine Funktionen.
[[nodiscard]] std::expected<VkDevice, std::string>
create_logical_device(const PhysicalDevice& physical_device, bool with_swapchain);

} // namespace ngine::rhi::vulkan
