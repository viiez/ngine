module;

#include "vk.hpp"

#include <format>
#include <string>
#include <string_view>

// Gemeinsame Hilfen des Vulkan-Backends: Fehlerbehandlung, Formatumrechnung und Loader.
// Interne Partition, außerhalb des Moduls nicht sichtbar.
module ngine.rhi.vulkan:common;

import ngine.core;
import ngine.rhi;

namespace ngine::rhi::vulkan {

// Anzahl der Frames, die die CPU der GPU vorausrechnen darf.
inline constexpr u32 frames_in_flight = 2;

// NGine setzt eine moderne GPU voraus (ADR 0014): Vulkan 1.4, Bindless, Buffer Device
// Address, Mesh Shader und Raytracing. Es gibt keine Rückfallpfade für ältere Hardware.
inline constexpr u32 required_api_version = VK_API_VERSION_1_4;

std::string_view result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "unbekannter VkResult";
    }
}

std::string failed(std::string_view what, VkResult result) {
    return std::format("{} fehlgeschlagen: {} ({})", what, result_name(result),
                       static_cast<int>(result));
}

// Bricht ab, wenn ein Vulkan-Aufruf scheitert, der im normalen Betrieb nicht scheitern darf.
void check(VkResult result, std::string_view what) {
    if (result != VK_SUCCESS) {
        verify(false, failed(what, result));
    }
}

std::string format_version(u32 version) {
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                       VK_API_VERSION_PATCH(version));
}

VkFormat to_vk(Format format) {
    switch (format) {
    case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::Unknown: break;
    }
    return VK_FORMAT_UNDEFINED;
}

Format from_vk(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8Unorm;
    case VK_FORMAT_B8G8R8A8_UNORM: return Format::BGRA8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB: return Format::RGBA8Srgb;
    case VK_FORMAT_B8G8R8A8_SRGB: return Format::BGRA8Srgb;
    default: return Format::Unknown;
    }
}

DeviceType from_vk(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return DeviceType::Integrated;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return DeviceType::Discrete;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return DeviceType::Virtual;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return DeviceType::Cpu;
    default: return DeviceType::Other;
    }
}

std::string_view to_string(DeviceType type) {
    switch (type) {
    case DeviceType::Integrated: return "integriert";
    case DeviceType::Discrete: return "dediziert";
    case DeviceType::Virtual: return "virtuell";
    case DeviceType::Cpu: return "CPU";
    case DeviceType::Other: break;
    }
    return "sonstige";
}

// --- Loader -----------------------------------------------------------------------------
// Mit volk kommen die Funktionszeiger direkt vom Treiber, sonst vom Vulkan-Loader.

bool load_vulkan() {
#if NGINE_VULKAN_USE_VOLK
    return volkInitialize() == VK_SUCCESS;
#else
    return true;
#endif
}

void load_instance_functions([[maybe_unused]] VkInstance instance) {
#if NGINE_VULKAN_USE_VOLK
    volkLoadInstanceOnly(instance);
#endif
}

void load_device_functions([[maybe_unused]] VkDevice device) {
#if NGINE_VULKAN_USE_VOLK
    volkLoadDevice(device);
#endif
}

} // namespace ngine::rhi::vulkan
