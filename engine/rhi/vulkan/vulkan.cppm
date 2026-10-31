module;

#include <memory>

// Vulkan-Backend der RHI: Vulkan 1.4 mit Dynamic Rendering, Bindless, Mesh Shadern und
// Raytracing (ADR 0014). Die Umsetzung steckt in internen Partitionen:
//   :common           Fehlerbehandlung, Formate, Loader
//   :instance         Instanz, Debug-Schicht, Surface
//   :physical_device  GPU-Auswahl nach Mindestanforderung, logisches Gerät
//   :resources        Texturen, Shader, Pipelines
//   :command_list     Aufzeichnen von Befehlen, Layout-Übergänge
//   :swapchain        Swapchain und Präsentation
// device.cpp setzt sie zum Device zusammen.
export module ngine.rhi.vulkan;

import ngine.rhi;

export namespace ngine::rhi::vulkan {

// Erzeugt ein Vulkan-Device. Schlägt fehl, wenn keine GPU die Mindestanforderung erfüllt.
[[nodiscard]] Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc);

} // namespace ngine::rhi::vulkan
