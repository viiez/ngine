module;

#include "vk.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;
import :common;
import :instance;

namespace ngine::rhi::vulkan {

namespace {

constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";

// Der Loader muss nur die Instanz-Funktionen von 1.3 kennen; Device-Funktionen kommen direkt
// vom Treiber. So reicht z. B. der Loader 1.3.275 aus Ubuntu 24.04 und Linux Mint 22.
constexpr u32 required_loader_version = VK_API_VERSION_1_3;

template <typename Function>
Function instance_function(VkInstance instance, const char* name) {
    return reinterpret_cast<Function>(vkGetInstanceProcAddr(instance, name));
}

VKAPI_ATTR VkBool32 VKAPI_CALL
on_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                 [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT types,
                 const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        static_cast<std::atomic<u32>*>(user_data)->fetch_add(1, std::memory_order_relaxed);
        log::error("Vulkan: {}", data->pMessage);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        log::warn("Vulkan: {}", data->pMessage);
    } else {
        log::debug("Vulkan: {}", data->pMessage);
    }
    return VK_FALSE;
}

bool has_layer(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::ranges::any_of(layers, [&](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, name) == 0;
    });
}

bool has_instance_extension(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    return std::ranges::any_of(extensions, [&](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

} // namespace

std::expected<void, std::string> Instance::init(const DeviceDesc& desc,
                                                std::atomic<u32>& validation_errors) {
    u32 version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < required_loader_version) {
        return std::unexpected("Der Vulkan-Loader ist älter als Vulkan 1.3");
    }

    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    if (desc.surface) {
        extensions = desc.surface->instance_extensions;
    }

    bool validation = false;
    if (desc.enable_validation) {
        if (has_layer(validation_layer) &&
            has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            layers.push_back(validation_layer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            validation = true;
        } else {
            log::warn("Vulkan Validation Layers sind nicht installiert, Validierung ist aus");
        }
    }

    const VkDebugUtilsMessengerCreateInfoEXT messenger_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = on_debug_message,
        .pUserData = &validation_errors,
    };

    const VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = desc.application_name.c_str(),
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = engine_name.data(),
        .engineVersion = VK_MAKE_API_VERSION(0, engine_version.major, engine_version.minor,
                                             engine_version.patch),
        .apiVersion = required_api_version,
    };
    const VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        // Meldungen schon beim Erzeugen und Zerstören der Instanz abfangen.
        .pNext = validation ? &messenger_info : nullptr,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = static_cast<u32>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (const VkResult result = vkCreateInstance(&instance_info, nullptr, &instance_);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateInstance", result));
    }
    load_instance_functions(instance_);

    if (validation) {
        const auto create_messenger = instance_function<PFN_vkCreateDebugUtilsMessengerEXT>(
            instance_, "vkCreateDebugUtilsMessengerEXT");
        if (create_messenger != nullptr) {
            create_messenger(instance_, &messenger_info, nullptr, &messenger_);
        }
        log::info("Vulkan Validation Layers aktiv");
    }
    return {};
}

std::expected<void, std::string> Instance::create_surface(const SurfaceDesc& surface) {
    const u64 handle = surface.create_surface(instance_);
    if (handle == 0) {
        return std::unexpected("Vulkan-Surface lässt sich nicht erzeugen");
    }
    surface_ = std::bit_cast<VkSurfaceKHR>(handle);
    return {};
}

Instance::~Instance() {
    if (instance_ == VK_NULL_HANDLE) {
        return;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE) {
        const auto destroy_messenger = instance_function<PFN_vkDestroyDebugUtilsMessengerEXT>(
            instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_messenger != nullptr) {
            destroy_messenger(instance_, messenger_, nullptr);
        }
    }
    vkDestroyInstance(instance_, nullptr);
}

} // namespace ngine::rhi::vulkan
