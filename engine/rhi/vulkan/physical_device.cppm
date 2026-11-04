module;

#include "vk.hpp"

#include <expected>
#include <string>

// Auswahl der GPU nach der Mindestanforderung (ADR 0017) und Erzeugen des logischen Geräts.
module ngine.rhi.vulkan:physical_device;

import ngine.core;
import ngine.rhi;

namespace ngine::rhi::vulkan {

struct PhysicalDevice {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    // Queue-Familie für Grafik und, mit Surface, Präsentation.
    u32 queue_family = 0;
    // Vulkan-Version des Devices: die der GPU, höchstens newest_api_version.
    u32 api_version = 0;
    // Enthält auch die optionalen Stufen, die create_logical_device einschaltet.
    Capabilities capabilities;
    // Gültige Bits der Zeitstempel der Queue-Familie (0: keine Zeitstempel).
    u32 timestamp_bits = 0;
};

// Wählt die beste GPU, die die Mindestanforderung erfüllt; dedizierte GPUs vor integrierten,
// bei gleichem Typ die mit mehr optionalen Stufen. Mit surface muss die Queue-Familie auch
// präsentieren können. Stufen in disabled bleiben aus.
[[nodiscard]] std::expected<PhysicalDevice, std::string>
pick_physical_device(VkInstance instance, VkSurfaceKHR surface, const Features& disabled);

// Erzeugt das logische Gerät mit allen Pflicht-Features und den erkannten optionalen Stufen
// und lädt seine Funktionen.
[[nodiscard]] std::expected<VkDevice, std::string>
create_logical_device(const PhysicalDevice& physical_device, bool with_swapchain);

} // namespace ngine::rhi::vulkan
