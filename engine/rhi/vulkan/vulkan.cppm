module;

#include <memory>

// Vulkan-Backend der RHI: Vulkan 1.3 mit Dynamic Rendering und Synchronization2.
export module ngine.rhi.vulkan;

import ngine.rhi;

export namespace ngine::rhi::vulkan {

// Erzeugt ein Vulkan-Device. Schlägt fehl, wenn kein Vulkan-Treiber mit Version 1.3
// und den nötigen Features vorhanden ist.
[[nodiscard]] Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc);

} // namespace ngine::rhi::vulkan
