module;

#include <memory>

// Vulkan-Backend der RHI. Pflicht ist Vulkan 1.3 mit Dynamic Rendering, Synchronization2 und
// Bindless; Mesh Shader und Raytracing sind optionale Stufen, die beim Start erkannt werden
// (ADR 0017). Exportiert wird nur die Auswertung der Anforderungen:
//   :requirements     Mindestanforderung und optionale Stufen, ohne GPU testbar
// Die Umsetzung steckt in internen Partitionen:
//   :common           Fehlerbehandlung, Formate, Loader
//   :instance         Instanz, Debug-Schicht, Surface
//   :physical_device  GPU-Auswahl, Abfrage der Fähigkeiten, logisches Gerät
//   :resources        Texturen, Shader, Pipelines
//   :command_list     Aufzeichnen von Befehlen, Layout-Übergänge
//   :swapchain        Swapchain und Präsentation
// device.cpp setzt sie zum Device zusammen.
export module ngine.rhi.vulkan;

import ngine.rhi;

export import :requirements;

export namespace ngine::rhi::vulkan {

// Erzeugt ein Vulkan-Device. Schlägt fehl, wenn keine GPU die Mindestanforderung erfüllt.
[[nodiscard]] Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc);

} // namespace ngine::rhi::vulkan
