module;

#include "vk.hpp"

#include <expected>
#include <optional>
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
    // Familie nur für Kopien (ohne Grafik und Compute), wie sie dedizierte GPUs für ihre
    // DMA-Einheiten haben (TD-17); nichts, wenn es keine gibt oder sie abgeschaltet ist.
    std::optional<u32> transfer_family;
    // Vulkan-Version des Devices: die der GPU, höchstens newest_api_version.
    u32 api_version = 0;
    // Enthält auch die optionalen Stufen, die create_logical_device einschaltet.
    Capabilities capabilities;
    // Gültige Bits der Zeitstempel der Queue-Familie (0: keine Zeitstempel).
    u32 timestamp_bits = 0;
    // Present-Fences (VK_EXT_swapchain_maintenance1, TD-18).
    bool swapchain_maintenance1 = false;
    // Zeitstempel der GPU ohne Warten (VK_KHR_ bzw. VK_EXT_calibrated_timestamps, TD-22): Name
    // der Extension oder nullptr.
    const char* calibrated_timestamps = nullptr;
};

// Wählt die beste GPU, die die Mindestanforderung erfüllt; dedizierte GPUs vor integrierten,
// bei gleichem Typ die mit mehr optionalen Stufen. Mit surface muss die Queue-Familie auch
// präsentieren können. Stufen in disabled bleiben aus. surface_maintenance1: die Instanz hat
// VK_EXT_surface_maintenance1 eingeschaltet. transfer_queue: eine eigene Transfer-Familie suchen.
// bc_compression: BC7 einschalten, wo die GPU es kann.
[[nodiscard]] std::expected<PhysicalDevice, std::string>
pick_physical_device(VkInstance instance, VkSurfaceKHR surface, const Features& disabled,
                     bool surface_maintenance1, bool transfer_queue, bool bc_compression);

// Erzeugt das logische Gerät mit allen Pflicht-Features und den erkannten optionalen Stufen
// und lädt seine Funktionen.
[[nodiscard]] std::expected<VkDevice, std::string>
create_logical_device(const PhysicalDevice& physical_device, bool with_swapchain);

} // namespace ngine::rhi::vulkan
