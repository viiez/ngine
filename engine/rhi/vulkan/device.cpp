module;

#include "vk.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;

namespace ngine::rhi::vulkan {

namespace {

constexpr u32 frames_in_flight = 2;
constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";

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

template <typename Function>
Function instance_function(VkInstance instance, const char* name) {
    return reinterpret_cast<Function>(vkGetInstanceProcAddr(instance, name));
}

// --- Debug-Schicht ------------------------------------------------------------------------

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

// --- Ressourcen -------------------------------------------------------------------------

class VulkanTexture final : public Texture {
public:
    VulkanTexture(VkDevice device, VmaAllocator allocator, VkImage image, VmaAllocation allocation,
                  VkImageView view, Extent2D extent, Format format)
        : device_{device}, allocator_{allocator}, image_{image}, allocation_{allocation},
          view_{view}, extent_{extent}, format_{format} {}

    ~VulkanTexture() override {
        vkDestroyImageView(device_, view_, nullptr);
        // Swapchain-Bilder gehören der Swapchain und haben keine eigene Allokation.
        if (allocation_ != nullptr) {
            vmaDestroyImage(allocator_, image_, allocation_);
        }
    }

    [[nodiscard]] Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] Format format() const override {
        return format_;
    }

    [[nodiscard]] VkImage image() const {
        return image_;
    }
    [[nodiscard]] VkImageView view() const {
        return view_;
    }

    // Aktuelles Layout, damit Übergänge automatisch eingefügt werden können.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

private:
    VkDevice device_;
    VmaAllocator allocator_;
    VkImage image_;
    VmaAllocation allocation_;
    VkImageView view_;
    Extent2D extent_;
    Format format_;
};

class VulkanShader final : public Shader {
public:
    VulkanShader(VkDevice device, VkShaderModule module, ShaderStage stage, std::string entry_point)
        : device_{device}, module_{module}, stage_{stage}, entry_point_{std::move(entry_point)} {}

    ~VulkanShader() override {
        vkDestroyShaderModule(device_, module_, nullptr);
    }

    [[nodiscard]] VkShaderModule module() const {
        return module_;
    }
    [[nodiscard]] ShaderStage stage() const {
        return stage_;
    }
    [[nodiscard]] const std::string& entry_point() const {
        return entry_point_;
    }

private:
    VkDevice device_;
    VkShaderModule module_;
    ShaderStage stage_;
    std::string entry_point_;
};

class VulkanPipeline final : public Pipeline {
public:
    VulkanPipeline(VkDevice device, VkPipelineLayout layout, VkPipeline pipeline)
        : device_{device}, layout_{layout}, pipeline_{pipeline} {}

    ~VulkanPipeline() override {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }

    [[nodiscard]] VkPipeline handle() const {
        return pipeline_;
    }

private:
    VkDevice device_;
    VkPipelineLayout layout_;
    VkPipeline pipeline_;
};

// Grober, aber sicherer Übergang zwischen zwei Layouts. Feinere Barrieren übernimmt
// später der Render-Graph.
void transition(VkCommandBuffer cmd, VulkanTexture& texture, VkImageLayout new_layout) {
    if (texture.layout == new_layout) {
        return;
    }
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = texture.layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture.image(),
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
    texture.layout = new_layout;
}

class VulkanCommandList final : public CommandList {
public:
    void reset(VkCommandBuffer cmd) {
        cmd_ = cmd;
    }
    [[nodiscard]] VkCommandBuffer handle() const {
        return cmd_;
    }

    void begin_rendering(Texture& target, std::optional<Color> clear) override {
        auto& texture = static_cast<VulkanTexture&>(target);
        transition(cmd_, texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        const Color color = clear.value_or(Color{});
        const VkRenderingAttachmentInfo attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = texture.view(),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = {.float32 = {color.r, color.g, color.b, color.a}}},
        };
        const VkExtent2D extent{texture.extent().width, texture.extent().height};
        const VkRenderingInfo rendering{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, extent},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment,
        };
        vkCmdBeginRendering(cmd_, &rendering);

        // Negative Höhe dreht die y-Achse, damit sie wie in OpenGL und D3D nach oben zeigt.
        const auto width = static_cast<f32>(extent.width);
        const auto height = static_cast<f32>(extent.height);
        const VkViewport viewport{0.0f, height, width, -height, 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd_, 0, 1, &viewport);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
    }

    void end_rendering() override {
        vkCmdEndRendering(cmd_);
    }

    void bind_pipeline(const Pipeline& pipeline) override {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          static_cast<const VulkanPipeline&>(pipeline).handle());
    }

    void draw(u32 vertex_count, u32 instance_count) override {
        vkCmdDraw(cmd_, vertex_count, instance_count, 0, 0);
    }

private:
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
};

// --- Device -------------------------------------------------------------------------------

class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(const DeviceDesc& desc) : vsync_{desc.vsync} {}
    ~VulkanDevice() override;

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    std::expected<void, std::string> init(const DeviceDesc& desc);

    [[nodiscard]] const Capabilities& capabilities() const override {
        return capabilities_;
    }

    [[nodiscard]] Result<std::unique_ptr<Texture>> create_texture(const TextureDesc& desc) override;
    [[nodiscard]] Result<std::unique_ptr<Shader>> create_shader(const ShaderDesc& desc) override;
    [[nodiscard]] Result<std::unique_ptr<Pipeline>>
    create_pipeline(const GraphicsPipelineDesc& desc) override;

    [[nodiscard]] Format swapchain_format() const override {
        return swapchain_format_;
    }
    [[nodiscard]] std::optional<Frame> begin_frame() override;
    void end_frame() override;
    void resize(Extent2D extent) override;

    void execute(const std::function<void(CommandList&)>& record) override;
    [[nodiscard]] std::vector<u8> read_texture(Texture& texture) override;

    void wait_idle() override {
        vkDeviceWaitIdle(device_);
    }

    [[nodiscard]] u32 validation_errors() const override {
        return validation_errors_.load(std::memory_order_relaxed);
    }

private:
    struct FrameData {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
    };

    std::expected<void, std::string> create_instance(const DeviceDesc& desc);
    std::expected<void, std::string> pick_physical_device();
    std::expected<void, std::string> create_logical_device();
    std::expected<void, std::string> create_allocator();
    void create_frame_resources();
    std::expected<void, std::string> create_swapchain();
    void destroy_swapchain_resources();

    std::atomic<u32> validation_errors_{0};
    bool vsync_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    Capabilities capabilities_;

    // Swapchain
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    Format swapchain_format_ = Format::Unknown;
    Extent2D requested_extent_;
    bool swapchain_dirty_ = false;
    std::vector<std::unique_ptr<VulkanTexture>> backbuffers_;
    // Ein Semaphor pro Swapchain-Bild, weil die Präsentation es erst später freigibt.
    std::vector<VkSemaphore> render_finished_;

    std::array<FrameData, frames_in_flight> frames_{};
    u32 frame_index_ = 0;
    u32 image_index_ = 0;
    VulkanCommandList frame_commands_;

    VkCommandPool immediate_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer immediate_cmd_ = VK_NULL_HANDLE;
    VkFence immediate_fence_ = VK_NULL_HANDLE;
};

std::expected<void, std::string> VulkanDevice::init(const DeviceDesc& desc) {
    if (!load_vulkan()) {
        return std::unexpected(
            "Vulkan-Loader nicht gefunden (ist ein Vulkan-Treiber installiert?)");
    }
    if (desc.surface) {
        requested_extent_ = desc.surface->extent;
    }
    if (auto result = create_instance(desc); !result) {
        return result;
    }
    if (desc.surface) {
        const u64 surface = desc.surface->create_surface(instance_);
        if (surface == 0) {
            return std::unexpected("Vulkan-Surface lässt sich nicht erzeugen");
        }
        surface_ = std::bit_cast<VkSurfaceKHR>(surface);
    }
    if (auto result = pick_physical_device(); !result) {
        return result;
    }
    if (auto result = create_logical_device(); !result) {
        return result;
    }
    if (auto result = create_allocator(); !result) {
        return result;
    }
    create_frame_resources();
    if (surface_ != VK_NULL_HANDLE) {
        if (auto result = create_swapchain(); !result) {
            return result;
        }
    }

    log::info("Vulkan-Gerät: {} ({}), Vulkan {}", capabilities_.device_name,
              to_string(capabilities_.device_type), capabilities_.api_version);
    log::info("Feature-Stufen: Mesh Shader {}, Raytracing {}",
              capabilities_.mesh_shaders ? "ja" : "nein",
              capabilities_.ray_tracing ? "ja" : "nein");
    return {};
}

std::expected<void, std::string> VulkanDevice::create_instance(const DeviceDesc& desc) {
    u32 version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_3) {
        return std::unexpected("Der Vulkan-Loader unterstützt Vulkan 1.3 nicht");
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
        .pUserData = &validation_errors_,
    };

    const VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = desc.application_name.c_str(),
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = engine_name.data(),
        .engineVersion = VK_MAKE_API_VERSION(0, engine_version.major, engine_version.minor,
                                             engine_version.patch),
        .apiVersion = VK_API_VERSION_1_3,
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

std::expected<void, std::string> VulkanDevice::pick_physical_device() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }

        VkPhysicalDeviceVulkan13Features features13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &features13};
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE) {
            continue;
        }

        const auto extensions = device_extensions(candidate);
        if (surface_ != VK_NULL_HANDLE && !contains(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }

        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        std::optional<u32> family;
        for (u32 i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 present = VK_TRUE;
            if (surface_ != VK_NULL_HANDLE) {
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present);
            }
            if (present == VK_TRUE) {
                family = i;
                break;
            }
        }
        if (!family) {
            continue;
        }

        const DeviceType type = from_vk(properties.deviceType);
        const int score = type == DeviceType::Discrete     ? 4
                          : type == DeviceType::Integrated ? 3
                          : type == DeviceType::Virtual    ? 2
                                                           : 1;
        if (score <= best_score) {
            continue;
        }
        best_score = score;
        physical_device_ = candidate;
        queue_family_ = *family;
        capabilities_ = Capabilities{
            .device_name = properties.deviceName,
            .api_version = std::format("{}.{}.{}", VK_API_VERSION_MAJOR(properties.apiVersion),
                                       VK_API_VERSION_MINOR(properties.apiVersion),
                                       VK_API_VERSION_PATCH(properties.apiVersion)),
            .device_type = type,
            .mesh_shaders = contains(extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME),
            .ray_tracing = contains(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
                           contains(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
        };
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        return std::unexpected(
            "Keine GPU mit Vulkan 1.3, Dynamic Rendering und Synchronization2 gefunden");
    }
    return {};
}

std::expected<void, std::string> VulkanDevice::create_logical_device() {
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    const VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features13,
    };

    std::vector<const char*> extensions;
    if (surface_ != VK_NULL_HANDLE) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (const VkResult result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateDevice", result));
    }
    load_device_functions(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return {};
}

std::expected<void, std::string> VulkanDevice::create_allocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    const VmaAllocatorCreateInfo allocator_info{
        .physicalDevice = physical_device_,
        .device = device_,
        .pVulkanFunctions = &functions,
        .instance = instance_,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    if (const VkResult result = vmaCreateAllocator(&allocator_info, &allocator_);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateAllocator", result));
    }
    return {};
}

void VulkanDevice::create_frame_resources() {
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_,
    };
    const VkFenceCreateInfo signaled_fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                           .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    auto allocate = [&](VkCommandPool pool) {
        const VkCommandBufferAllocateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(device_, &info, &cmd), "vkAllocateCommandBuffers");
        return cmd;
    };

    for (FrameData& frame : frames_) {
        check(vkCreateCommandPool(device_, &pool_info, nullptr, &frame.pool),
              "vkCreateCommandPool");
        frame.cmd = allocate(frame.pool);
        check(vkCreateFence(device_, &signaled_fence, nullptr, &frame.in_flight), "vkCreateFence");
        check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.image_available),
              "vkCreateSemaphore");
    }

    check(vkCreateCommandPool(device_, &pool_info, nullptr, &immediate_pool_),
          "vkCreateCommandPool");
    immediate_cmd_ = allocate(immediate_pool_);
    const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device_, &fence_info, nullptr, &immediate_fence_), "vkCreateFence");
}

std::expected<void, std::string> VulkanDevice::create_swapchain() {
    VkSurfaceCapabilitiesKHR surface_caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &surface_caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    VkExtent2D extent = surface_caps.currentExtent;
    if (extent.width == std::numeric_limits<u32>::max()) {
        extent.width = std::clamp(requested_extent_.width, surface_caps.minImageExtent.width,
                                  surface_caps.maxImageExtent.width);
        extent.height = std::clamp(requested_extent_.height, surface_caps.minImageExtent.height,
                                   surface_caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        // Minimiertes Fenster: später erneut versuchen.
        swapchain_dirty_ = true;
        return {};
    }

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());
    if (formats.empty()) {
        return std::unexpected("Die Surface bietet keine Formate an");
    }
    VkSurfaceFormatKHR surface_format = formats.front();
    for (const VkFormat preferred : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
        const auto match = std::ranges::find_if(formats, [&](const VkSurfaceFormatKHR& f) {
            return f.format == preferred && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (match != formats.end()) {
            surface_format = *match;
            break;
        }
    }

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        u32 mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count,
                                                  modes.data());
        for (const VkPresentModeKHR preferred :
             {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::ranges::find(modes, preferred) != modes.end()) {
                present_mode = preferred;
                break;
            }
        }
    }

    u32 image_count = surface_caps.minImageCount + 1;
    if (surface_caps.maxImageCount > 0) {
        image_count = std::min(image_count, surface_caps.maxImageCount);
    }

    VkSwapchainKHR old_swapchain = swapchain_;
    const VkSwapchainCreateInfoKHR swapchain_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface_,
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = surface_caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain,
    };
    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    if (const VkResult result =
            vkCreateSwapchainKHR(device_, &swapchain_info, nullptr, &new_swapchain);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateSwapchainKHR", result));
    }

    destroy_swapchain_resources();
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, old_swapchain, nullptr);
    }
    swapchain_ = new_swapchain;
    swapchain_format_ = from_vk(surface_format.format);
    swapchain_dirty_ = false;

    u32 count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, images.data());

    const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkImage image : images) {
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = surface_format.format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        check(vkCreateImageView(device_, &view_info, nullptr, &view), "vkCreateImageView");
        backbuffers_.push_back(std::make_unique<VulkanTexture>(
            device_, allocator_, image, nullptr, view, Extent2D{extent.width, extent.height},
            swapchain_format_));

        VkSemaphore semaphore = VK_NULL_HANDLE;
        check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore),
              "vkCreateSemaphore");
        render_finished_.push_back(semaphore);
    }

    log::debug("Swapchain: {}x{}, {} Bilder", extent.width, extent.height, count);
    return {};
}

void VulkanDevice::destroy_swapchain_resources() {
    backbuffers_.clear();
    for (VkSemaphore semaphore : render_finished_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_finished_.clear();
}

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain_resources();
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        }
        for (FrameData& frame : frames_) {
            vkDestroySemaphore(device_, frame.image_available, nullptr);
            vkDestroyFence(device_, frame.in_flight, nullptr);
            vkDestroyCommandPool(device_, frame.pool, nullptr);
        }
        vkDestroyFence(device_, immediate_fence_, nullptr);
        vkDestroyCommandPool(device_, immediate_pool_, nullptr);
        if (allocator_ != nullptr) {
            vmaDestroyAllocator(allocator_);
        }
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
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
}

Result<std::unique_ptr<Texture>> VulkanDevice::create_texture(const TextureDesc& desc) {
    const VkFormat format = to_vk(desc.format);
    if (format == VK_FORMAT_UNDEFINED || desc.extent.width == 0 || desc.extent.height == 0) {
        return std::unexpected("Ungültige Textur-Beschreibung");
    }

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {desc.extent.width, desc.extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO};
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    if (const VkResult result =
            vmaCreateImage(allocator_, &image_info, &allocation_info, &image, &allocation, nullptr);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateImage", result));
    }

    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkImageView view = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateImageView(device_, &view_info, nullptr, &view);
        result != VK_SUCCESS) {
        vmaDestroyImage(allocator_, image, allocation);
        return std::unexpected(failed("vkCreateImageView", result));
    }
    return std::make_unique<VulkanTexture>(device_, allocator_, image, allocation, view,
                                           desc.extent, desc.format);
}

Result<std::unique_ptr<Shader>> VulkanDevice::create_shader(const ShaderDesc& desc) {
    if (desc.spirv.empty()) {
        return std::unexpected("Shader ohne SPIR-V-Code");
    }
    const VkShaderModuleCreateInfo module_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = desc.spirv.size_bytes(),
        .pCode = desc.spirv.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateShaderModule(device_, &module_info, nullptr, &module);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateShaderModule", result));
    }
    return std::make_unique<VulkanShader>(device_, module, desc.stage, desc.entry_point);
}

Result<std::unique_ptr<Pipeline>> VulkanDevice::create_pipeline(const GraphicsPipelineDesc& desc) {
    if (desc.vertex == nullptr || desc.fragment == nullptr) {
        return std::unexpected("Pipeline braucht Vertex- und Fragment-Shader");
    }
    const VkFormat color_format = to_vk(desc.color_format);
    if (color_format == VK_FORMAT_UNDEFINED) {
        return std::unexpected("Pipeline braucht ein Farbformat");
    }

    const auto& vertex = static_cast<const VulkanShader&>(*desc.vertex);
    const auto& fragment = static_cast<const VulkanShader&>(*desc.fragment);
    const std::array stages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex.module(),
            .pName = vertex.entry_point().c_str(),
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment.module(),
            .pName = fragment.entry_point().c_str(),
        },
    };

    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState blend_attachment{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };

    const VkPipelineLayoutCreateInfo layout_info{.sType =
                                                     VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (const VkResult result = vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreatePipelineLayout", result));
    }

    const VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                                          &pipeline_info, nullptr, &pipeline);
        result != VK_SUCCESS) {
        vkDestroyPipelineLayout(device_, layout, nullptr);
        return std::unexpected(failed("vkCreateGraphicsPipelines", result));
    }
    return std::make_unique<VulkanPipeline>(device_, layout, pipeline);
}

std::optional<Frame> VulkanDevice::begin_frame() {
    if (surface_ == VK_NULL_HANDLE) {
        return std::nullopt;
    }
    if (swapchain_ == VK_NULL_HANDLE || swapchain_dirty_) {
        vkDeviceWaitIdle(device_);
        if (auto result = create_swapchain(); !result) {
            log::error("{}", result.error());
            return std::nullopt;
        }
        if (swapchain_dirty_) {
            return std::nullopt;
        }
    }

    FrameData& frame = frames_[frame_index_];
    check(vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, std::numeric_limits<u64>::max()),
          "vkWaitForFences");

    const VkResult acquired =
        vkAcquireNextImageKHR(device_, swapchain_, std::numeric_limits<u64>::max(),
                              frame.image_available, VK_NULL_HANDLE, &image_index_);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_dirty_ = true;
        return std::nullopt;
    }
    if (acquired == VK_SUBOPTIMAL_KHR) {
        swapchain_dirty_ = true;
    } else {
        check(acquired, "vkAcquireNextImageKHR");
    }

    // Erst nach erfolgreichem Acquire zurücksetzen, sonst wartet der nächste Frame ewig.
    check(vkResetFences(device_, 1, &frame.in_flight), "vkResetFences");
    check(vkResetCommandBuffer(frame.cmd, 0), "vkResetCommandBuffer");
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(frame.cmd, &begin_info), "vkBeginCommandBuffer");
    frame_commands_.reset(frame.cmd);

    VulkanTexture& backbuffer = *backbuffers_[image_index_];
    // Der alte Inhalt wird nicht gebraucht; das spart einen Übergang mit Kopie.
    backbuffer.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return Frame{frame_commands_, backbuffer};
}

void VulkanDevice::end_frame() {
    FrameData& frame = frames_[frame_index_];
    VulkanTexture& backbuffer = *backbuffers_[image_index_];
    transition(frame.cmd, backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    check(vkEndCommandBuffer(frame.cmd), "vkEndCommandBuffer");

    const VkSemaphoreSubmitInfo wait{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = frame.image_available,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkSemaphoreSubmitInfo signal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = render_finished_[image_index_],
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkCommandBufferSubmitInfo command{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = frame.cmd,
    };
    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };
    check(vkQueueSubmit2(queue_, 1, &submit, frame.in_flight), "vkQueueSubmit2");

    const VkPresentInfoKHR present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_finished_[image_index_],
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &image_index_,
    };
    const VkResult presented = vkQueuePresentKHR(queue_, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        swapchain_dirty_ = true;
    } else {
        check(presented, "vkQueuePresentKHR");
    }

    frame_index_ = (frame_index_ + 1) % frames_in_flight;
}

void VulkanDevice::resize(Extent2D extent) {
    requested_extent_ = extent;
    swapchain_dirty_ = true;
}

void VulkanDevice::execute(const std::function<void(CommandList&)>& record) {
    check(vkResetCommandBuffer(immediate_cmd_, 0), "vkResetCommandBuffer");
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(immediate_cmd_, &begin_info), "vkBeginCommandBuffer");

    VulkanCommandList commands;
    commands.reset(immediate_cmd_);
    record(commands);

    check(vkEndCommandBuffer(immediate_cmd_), "vkEndCommandBuffer");
    const VkCommandBufferSubmitInfo command{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = immediate_cmd_,
    };
    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command,
    };
    check(vkQueueSubmit2(queue_, 1, &submit, immediate_fence_), "vkQueueSubmit2");
    check(vkWaitForFences(device_, 1, &immediate_fence_, VK_TRUE, std::numeric_limits<u64>::max()),
          "vkWaitForFences");
    check(vkResetFences(device_, 1, &immediate_fence_), "vkResetFences");
}

std::vector<u8> VulkanDevice::read_texture(Texture& texture) {
    auto& source = static_cast<VulkanTexture&>(texture);
    const Extent2D extent = source.extent();
    const VkDeviceSize size =
        VkDeviceSize{extent.width} * extent.height * bytes_per_pixel(source.format());

    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation_info{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo allocated{};
    check(vmaCreateBuffer(allocator_, &buffer_info, &allocation_info, &buffer, &allocation,
                          &allocated),
          "vmaCreateBuffer");

    execute([&](CommandList& commands) {
        const VkCommandBuffer cmd = static_cast<VulkanCommandList&>(commands).handle();
        transition(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkBufferImageCopy region{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {extent.width, extent.height, 1},
        };
        vkCmdCopyImageToBuffer(cmd, source.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                               &region);
        // Kopierte Daten für die CPU sichtbar machen.
        const VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &dependency);
    });

    check(vmaInvalidateAllocation(allocator_, allocation, 0, VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    std::vector<u8> pixels(static_cast<usize>(size));
    std::memcpy(pixels.data(), allocated.pMappedData, pixels.size());
    vmaDestroyBuffer(allocator_, buffer, allocation);
    return pixels;
}

} // namespace

Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc) {
    auto device = std::make_unique<VulkanDevice>(desc);
    if (auto result = device->init(desc); !result) {
        return std::unexpected(result.error());
    }
    return device;
}

} // namespace ngine::rhi::vulkan
