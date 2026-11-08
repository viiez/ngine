module;

#include "vk.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;
import :common;
import :physical_device;

namespace ngine::rhi::vulkan {

namespace {

static_assert(minimum_api_version == VK_API_VERSION_1_3);
static_assert(newest_api_version == VK_API_VERSION_1_4);

// Verkettete Feature-Strukturen. Dieselbe Kette dient zum Abfragen und zum Einschalten;
// die Zeiger zeigen in das Objekt selbst, deshalb ist es weder kopier- noch verschiebbar.
// Die Strukturen optionaler Stufen hängen nur in der Kette, wenn die GPU deren Extensions
// hat: Strukturen unbekannter Extensions darf man weder abfragen noch übergeben.
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

    FeatureChain(bool with_mesh_shader, bool with_ray_tracing) {
        root.pNext = &v11;
        v11.pNext = &v12;
        v12.pNext = &v13;
        void** tail = &v13.pNext;
        if (with_mesh_shader) {
            *tail = &mesh;
            tail = &mesh.pNext;
        }
        if (with_ray_tracing) {
            *tail = &acceleration;
            acceleration.pNext = &ray_query;
            tail = &ray_query.pNext;
        }
    }
    FeatureChain(const FeatureChain&) = delete;
    FeatureChain& operator=(const FeatureChain&) = delete;
};

// Liefert die Felder aller Pflicht-Features einer Kette, damit Abfrage und Einschalten
// dieselbe Liste benutzen. Die Liste entspricht ADR 0017; Intel-, AMD- und NVIDIA-Treiber
// erfüllen sie ab Vulkan 1.3.
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
    // Bindloser Satz mit Storage-Texturen, die sich ändern, während Frames im Flug sind (ADR 0026).
    visit("Storage Image Update After Bind",
          chain.v12.descriptorBindingStorageImageUpdateAfterBind);
    visit("Update Unused While Pending", chain.v12.descriptorBindingUpdateUnusedWhilePending);
    visit("Non-Uniform Storage Image Indexing",
          chain.v12.shaderStorageImageArrayNonUniformIndexing);
    // Storage-Texturen ohne festes Format im Shader (RWTexture2D<float4>).
    visit("Storage Image Read Without Format",
          chain.root.features.shaderStorageImageReadWithoutFormat);
    visit("Storage Image Write Without Format",
          chain.root.features.shaderStorageImageWriteWithoutFormat);
    visit("Buffer Device Address", chain.v12.bufferDeviceAddress);
    visit("Timeline Semaphores", chain.v12.timelineSemaphore);
    visit("Scalar Block Layout", chain.v12.scalarBlockLayout);
    visit("Dynamic Rendering", chain.v13.dynamicRendering);
    visit("Synchronization2", chain.v13.synchronization2);
    visit("Maintenance4", chain.v13.maintenance4);
}

std::vector<std::string> device_extensions(VkPhysicalDevice device) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> properties(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data());
    std::vector<std::string> names;
    names.reserve(count);
    for (const VkExtensionProperties& extension : properties) {
        names.emplace_back(extension.extensionName);
    }
    return names;
}

bool contains(const std::vector<std::string>& extensions, std::string_view name) {
    return std::ranges::find(extensions, name) != extensions.end();
}

// Fragt alles ab, was evaluate_device braucht.
DeviceSupport query_support(VkPhysicalDevice device, const VkPhysicalDeviceProperties& properties) {
    DeviceSupport support{
        .api_version = properties.apiVersion,
        .extensions = device_extensions(device),
    };
    // Die Strukturen von Vulkan 1.3 darf nur eine GPU mit Vulkan 1.3 bekommen.
    if (properties.apiVersion < minimum_api_version) {
        return support;
    }

    const bool has_mesh = contains(support.extensions, mesh_shader_extension);
    const bool has_ray_tracing = contains(support.extensions, acceleration_structure_extension) &&
                                 contains(support.extensions, deferred_host_operations_extension) &&
                                 contains(support.extensions, ray_query_extension);
    FeatureChain chain{has_mesh, has_ray_tracing};
    vkGetPhysicalDeviceFeatures2(device, &chain.root);

    for_each_required_feature(chain, [&](std::string_view name, const VkBool32& value) {
        support.required_features.push_back({.name = name, .supported = value == VK_TRUE});
    });
    support.task_shader = has_mesh && chain.mesh.taskShader == VK_TRUE;
    support.mesh_shader = has_mesh && chain.mesh.meshShader == VK_TRUE;
    support.acceleration_structure =
        has_ray_tracing && chain.acceleration.accelerationStructure == VK_TRUE;
    support.ray_query = has_ray_tracing && chain.ray_query.rayQuery == VK_TRUE;
    return support;
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

// Familie nur für Kopien: Transfer ohne Grafik und Compute, Kopien beliebiger Rechtecke
// (Granularität 1 × 1 × 1).
std::optional<u32> find_transfer_family(VkPhysicalDevice device) {
    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
    for (u32 i = 0; i < family_count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        const VkExtent3D granularity = families[i].minImageTransferGranularity;
        if ((flags & VK_QUEUE_TRANSFER_BIT) != 0 &&
            (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0 &&
            families[i].queueCount > 0 && granularity.width == 1 && granularity.height == 1 &&
            granularity.depth == 1) {
            return i;
        }
    }
    return std::nullopt;
}

// Kann die GPU ihre Zeitstempel ohne Abfrage in der Queue liefern (Zeitbereich Device)?
bool device_time_domain(VkPhysicalDevice device, const char* extension) {
    const bool khr = std::string_view{extension} == VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    const PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR query =
        khr ? vkGetPhysicalDeviceCalibrateableTimeDomainsKHR
            : vkGetPhysicalDeviceCalibrateableTimeDomainsEXT;
    if (query == nullptr) {
        return false;
    }
    u32 count = 0;
    if (query(device, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkTimeDomainKHR> domains(count);
    if (query(device, &count, domains.data()) != VK_SUCCESS) {
        return false;
    }
    return std::ranges::find(domains, VK_TIME_DOMAIN_DEVICE_KHR) != domains.end();
}

// Optionale Extensions, die keine Stufe sind: Present-Fences und kalibrierte Zeitstempel.
void detect_extras(PhysicalDevice& device, bool surface_maintenance1) {
    const std::vector<std::string> extensions = device_extensions(device.handle);
    if (surface_maintenance1 &&
        contains(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &maintenance};
        vkGetPhysicalDeviceFeatures2(device.handle, &features);
        device.swapchain_maintenance1 = maintenance.swapchainMaintenance1 == VK_TRUE;
    }
    if (device.timestamp_bits > 0) {
        for (const char* name : {VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
                                 VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME}) {
            if (contains(extensions, name) && device_time_domain(device.handle, name)) {
                device.calibrated_timestamps = name;
                break;
            }
        }
    }
}

// BC7 als Textur zum Lesen mit linearem Filter, in beiden Farbräumen (R2.5).
bool supports_bc7(VkPhysicalDevice device, const VkPhysicalDeviceFeatures& basic) {
    if (basic.textureCompressionBC != VK_TRUE) {
        return false;
    }
    constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (const VkFormat format : {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device, format, &properties);
        if ((properties.optimalTilingFeatures & needed) != needed) {
            return false;
        }
    }
    return true;
}

} // namespace

std::expected<PhysicalDevice, std::string>
pick_physical_device(VkInstance instance, VkSurfaceKHR surface, const Features& disabled,
                     bool surface_maintenance1, bool transfer_queue, bool bc_compression) {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    std::optional<PhysicalDevice> best;
    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        const DeviceEvaluation evaluation = evaluate_device(query_support(candidate, properties),
                                                            surface != VK_NULL_HANDLE, disabled);
        if (!evaluation.suitable()) {
            std::string list;
            for (const auto& name : evaluation.missing) {
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
        const int score = device_score(type, evaluation.features);
        if (score <= best_score) {
            continue;
        }
        best_score = score;
        VkPhysicalDeviceFeatures basic{};
        vkGetPhysicalDeviceFeatures(candidate, &basic);
        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        const u32 timestamp_bits = families[*family].timestampValidBits;
        const std::optional<u32> transfer =
            transfer_queue ? find_transfer_family(candidate) : std::nullopt;
        best = PhysicalDevice{
            .handle = candidate,
            .queue_family = *family,
            .transfer_family = transfer,
            .api_version = evaluation.api_version,
            .capabilities =
                Capabilities{
                    .device_name = properties.deviceName,
                    .api_version = format_version(properties.apiVersion),
                    .device_type = type,
                    .features = evaluation.features,
                    .max_anisotropy = basic.samplerAnisotropy == VK_TRUE
                                          ? properties.limits.maxSamplerAnisotropy
                                          : 1.0f,
                    .timestamp_period =
                        timestamp_bits > 0 ? properties.limits.timestampPeriod : 0.0f,
                    .transfer_queue = transfer.has_value(),
                    .bc_compression = bc_compression && supports_bc7(candidate, basic),
                },
            .timestamp_bits = timestamp_bits,
        };
    }

    if (best) {
        detect_extras(*best, surface_maintenance1);
    }
    if (!best) {
        return std::unexpected("Keine GPU erfüllt die Mindestanforderung (Vulkan 1.3 mit Dynamic "
                               "Rendering, Synchronization2 und Bindless); Details stehen im Log");
    }
    return *std::move(best);
}

std::expected<VkDevice, std::string> create_logical_device(const PhysicalDevice& physical_device,
                                                           bool with_swapchain) {
    const float priority = 1.0f;
    std::array<VkDeviceQueueCreateInfo, 2> queue_infos{};
    u32 queue_count = 0;
    queue_infos[queue_count++] = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = physical_device.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    if (physical_device.transfer_family) {
        queue_infos[queue_count++] = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = *physical_device.transfer_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
    }

    const Features& enabled = physical_device.capabilities.features;
    FeatureChain features{enabled.mesh_shader, enabled.ray_tracing};
    for_each_required_feature(features, [](std::string_view, VkBool32& value) { value = VK_TRUE; });
    if (physical_device.capabilities.max_anisotropy > 1.0f) {
        features.root.features.samplerAnisotropy = VK_TRUE;
    }
    if (physical_device.capabilities.bc_compression) {
        features.root.features.textureCompressionBC = VK_TRUE;
    }

    std::vector<const char*> extensions;
    if (enabled.mesh_shader) {
        features.mesh.taskShader = VK_TRUE;
        features.mesh.meshShader = VK_TRUE;
        extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    }
    if (enabled.ray_tracing) {
        features.acceleration.accelerationStructure = VK_TRUE;
        features.ray_query.rayQuery = VK_TRUE;
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    if (with_swapchain) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if (physical_device.swapchain_maintenance1) {
            extensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
            maintenance.swapchainMaintenance1 = VK_TRUE;
            maintenance.pNext = features.root.pNext;
            features.root.pNext = &maintenance;
        }
    }
    if (physical_device.calibrated_timestamps != nullptr) {
        extensions.push_back(physical_device.calibrated_timestamps);
    }

    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features.root,
        .queueCreateInfoCount = queue_count,
        .pQueueCreateInfos = queue_infos.data(),
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
