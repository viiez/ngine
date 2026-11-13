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
    case Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::R32Float: return VK_FORMAT_R32_SFLOAT;
    case Format::R32Uint: return VK_FORMAT_R32_UINT;
    case Format::D32Float: return VK_FORMAT_D32_SFLOAT;
    case Format::BC7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case Format::BC7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
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
    case VK_FORMAT_R16G16B16A16_SFLOAT: return Format::RGBA16Float;
    case VK_FORMAT_R32_SFLOAT: return Format::R32Float;
    case VK_FORMAT_R32_UINT: return Format::R32Uint;
    case VK_FORMAT_D32_SFLOAT: return Format::D32Float;
    case VK_FORMAT_BC7_UNORM_BLOCK: return Format::BC7Unorm;
    case VK_FORMAT_BC7_SRGB_BLOCK: return Format::BC7Srgb;
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

// --- Zustände ---------------------------------------------------------------------------

// Pipeline-Stufen, Zugriffe und Layout zu einem Zustand der RHI.
struct StateInfo {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Alle Shader-Stufen, in denen Ressourcen gelesen oder geschrieben werden können. ALL_GRAPHICS
// schließt Task- und Mesh-Shader ein, wenn sie eingeschaltet sind (R3.5), und ist auch ohne sie
// gültig.
inline constexpr VkPipelineStageFlags2 shader_stages =
    VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

StateInfo state_info(ResourceState state) {
    switch (state) {
    case ResourceState::Undefined:
        // Als Vorher-Zustand: Der Inhalt zählt nicht, aber frühere Arbeit (etwa ein anderer Frame
        // im Flug, der dieselbe Ressource benutzt hat) muss fertig sein, bevor sie neu beschrieben
        // wird. Deshalb wartet die Barriere auf alle Stufen und alle Schreibzugriffe.
        return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED};
    case ResourceState::ColorTarget:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    case ResourceState::DepthTarget:
        return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL};
    case ResourceState::DepthRead:
        return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    case ResourceState::ShaderRead:
        return {shader_stages,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    case ResourceState::ShaderWrite:
        return {shader_stages,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::IndexRead:
        return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED};
    case ResourceState::TransferSrc:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    case ResourceState::TransferDst:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
    case ResourceState::Present:
        // Die Präsentation wartet auf den Semaphor, den die Abgabe nach allen Stufen
        // signalisiert. Damit der Layout-Wechsel davor liegt, endet die Barriere ebenfalls mit
        // allen Stufen, ohne Speicherzugriff.
        return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    case ResourceState::HostRead:
        // Macht Schreibzugriffe der GPU für die CPU sichtbar; die CPU wartet über die Timeline.
        return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
    case ResourceState::IndirectRead:
        return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED};
    }
    return {};
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
