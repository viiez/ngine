module;

#include "vk.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <vector>

module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;
import :common;
import :physical_device;

namespace ngine::rhi::vulkan {

namespace {

// Device-Extensions, die jede GPU mitbringen muss (die Swapchain kommt nur mit Fenster dazu).
constexpr std::array required_extensions{
    VK_EXT_MESH_SHADER_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
};

// Verkettete Feature-Strukturen. Dieselbe Kette dient zum Abfragen und zum Einschalten;
// die Zeiger zeigen in das Objekt selbst, deshalb ist es weder kopier- noch verschiebbar.
struct FeatureChain {
    VkPhysicalDeviceFeatures2 root{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features v11{.sType =
                                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{.sType =
                                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{.sType =
                                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_pipeline{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};

    FeatureChain() {
        root.pNext = &v11;
        v11.pNext = &v12;
        v12.pNext = &v13;
        v13.pNext = &mesh;
        mesh.pNext = &acceleration;
        acceleration.pNext = &ray_query;
        ray_query.pNext = &ray_pipeline;
    }
    FeatureChain(const FeatureChain&) = delete;
    FeatureChain& operator=(const FeatureChain&) = delete;
};

// Liefert die Felder aller Pflicht-Features einer Kette, damit Abfrage und Einschalten
// dieselbe Liste benutzen.
template <typename Visitor>
void for_each_required_feature(FeatureChain& chain, Visitor&& visit) {
    // Slang übersetzt SV_VertexID mit der SPIR-V-Fähigkeit DrawParameters.
    visit("Draw Parameters", chain.v11.shaderDrawParameters);
    visit("Descriptor Indexing", chain.v12.descriptorIndexing);
    visit("Runtime Descriptor Arrays", chain.v12.runtimeDescriptorArray);
    visit("Partially Bound Descriptors", chain.v12.descriptorBindingPartiallyBound);
    visit("Variable Descriptor Count", chain.v12.descriptorBindingVariableDescriptorCount);
    visit("Non-Uniform Indexing", chain.v12.shaderSampledImageArrayNonUniformIndexing);
    visit("Update After Bind", chain.v12.descriptorBindingSampledImageUpdateAfterBind);
    visit("Buffer Device Address", chain.v12.bufferDeviceAddress);
    visit("Timeline Semaphores", chain.v12.timelineSemaphore);
    visit("Scalar Block Layout", chain.v12.scalarBlockLayout);
    visit("Dynamic Rendering", chain.v13.dynamicRendering);
    visit("Synchronization2", chain.v13.synchronization2);
    visit("Maintenance4", chain.v13.maintenance4);
    visit("Task Shader", chain.mesh.taskShader);
    visit("Mesh Shader", chain.mesh.meshShader);
    visit("Acceleration Structures", chain.acceleration.accelerationStructure);
    visit("Ray Query", chain.ray_query.rayQuery);
    visit("Raytracing-Pipeline", chain.ray_pipeline.rayTracingPipeline);
}

std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    return extensions;
}

bool contains(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::ranges::any_of(extensions, [&](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

// Was einer GPU zur Mindestanforderung fehlt; leer, wenn sie genügt.
std::vector<std::string> missing_requirements(VkPhysicalDevice device,
                                              const VkPhysicalDeviceProperties& properties,
                                              bool needs_swapchain) {
    std::vector<std::string> missing;
    if (properties.apiVersion < required_api_version) {
        missing.push_back(
            std::format("Vulkan 1.4 (hat {})", format_version(properties.apiVersion)));
        return missing;
    }

    const auto extensions = device_extensions(device);
    for (const char* name : required_extensions) {
        if (!contains(extensions, name)) {
            missing.emplace_back(name);
        }
    }
    if (needs_swapchain && !contains(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        missing.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (!missing.empty()) {
        // Feature-Strukturen fehlender Extensions dürfen nicht abgefragt werden.
        return missing;
    }

    FeatureChain chain;
    vkGetPhysicalDeviceFeatures2(device, &chain.root);
    for_each_required_feature(chain, [&](const char* name, const VkBool32& value) {
        if (value != VK_TRUE) {
            missing.emplace_back(name);
        }
    });
    return missing;
}

// Erste Queue-Familie mit Grafik und, falls verlangt, Präsentation auf surface.
std::optional<u32> find_queue_family(VkPhysicalDevice device, VkSurfaceKHR surface) {
    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());

    for (u32 i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        VkBool32 present = VK_TRUE;
        if (surface != VK_NULL_HANDLE) {
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present);
        }
        if (present == VK_TRUE) {
            return i;
        }
    }
    return std::nullopt;
}

int score(DeviceType type) {
    switch (type) {
    case DeviceType::Discrete: return 4;
    case DeviceType::Integrated: return 3;
    case DeviceType::Virtual: return 2;
    case DeviceType::Cpu:
    case DeviceType::Other: break;
    }
    return 1;
}

} // namespace

std::expected<PhysicalDevice, std::string> pick_physical_device(VkInstance instance,
                                                                VkSurfaceKHR surface) {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    std::optional<PhysicalDevice> best;
    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (const auto missing =
                missing_requirements(candidate, properties, surface != VK_NULL_HANDLE);
            !missing.empty()) {
            std::string list;
            for (const auto& name : missing) {
                list += list.empty() ? name : ", " + name;
            }
            log::info("GPU {} übersprungen, es fehlt: {}", properties.deviceName, list);
            continue;
        }

        const std::optional<u32> family = find_queue_family(candidate, surface);
        if (!family) {
            continue;
        }

        const DeviceType type = from_vk(properties.deviceType);
        if (score(type) <= best_score) {
            continue;
        }
        best_score = score(type);
        best = PhysicalDevice{
            .handle = candidate,
            .queue_family = *family,
            .capabilities =
                Capabilities{
                    .device_name = properties.deviceName,
                    .api_version = format_version(properties.apiVersion),
                    .device_type = type,
                },
        };
    }

    if (!best) {
        return std::unexpected("Keine GPU erfüllt die Mindestanforderung (Vulkan 1.4, Bindless, "
                               "Mesh Shader, Raytracing); Details stehen im Log");
    }
    return *std::move(best);
}

std::expected<VkDevice, std::string> create_logical_device(const PhysicalDevice& physical_device,
                                                           bool with_swapchain) {
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = physical_device.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    FeatureChain features;
    for_each_required_feature(features, [](const char*, VkBool32& value) { value = VK_TRUE; });

    std::vector<const char*> extensions(required_extensions.begin(), required_extensions.end());
    if (with_swapchain) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features.root,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    VkDevice device = VK_NULL_HANDLE;
    if (const VkResult result =
            vkCreateDevice(physical_device.handle, &device_info, nullptr, &device);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateDevice", result));
    }
    load_device_functions(device);
    return device;
}

} // namespace ngine::rhi::vulkan
