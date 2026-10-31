module;

#include "vk.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Das Vulkan-Device setzt die Teile des Backends zusammen: Instanz, GPU-Auswahl, Allokator,
// Frames, Swapchain und sofortige Ausführung.
module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;
import :common;
import :instance;
import :physical_device;
import :resources;
import :command_list;
import :swapchain;

namespace ngine::rhi::vulkan {

namespace {

class VulkanDevice final : public Device {
public:
    VulkanDevice() = default;
    ~VulkanDevice() override;

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    std::expected<void, std::string> init(const DeviceDesc& desc);

    [[nodiscard]] const Capabilities& capabilities() const override {
        return physical_device_.capabilities;
    }

    [[nodiscard]] Result<std::unique_ptr<Texture>>
    create_texture(const TextureDesc& desc) override {
        return vulkan::create_texture(device_, allocator_, desc);
    }
    [[nodiscard]] Result<std::unique_ptr<Shader>> create_shader(const ShaderDesc& desc) override {
        return vulkan::create_shader(device_, desc);
    }
    [[nodiscard]] Result<std::unique_ptr<Pipeline>>
    create_pipeline(const GraphicsPipelineDesc& desc) override {
        return vulkan::create_pipeline(device_, desc);
    }

    [[nodiscard]] Format swapchain_format() const override {
        return swapchain_ ? swapchain_->format() : Format::Unknown;
    }
    [[nodiscard]] std::optional<Frame> begin_frame() override;
    void end_frame() override;
    void resize(Extent2D extent) override {
        if (swapchain_) {
            swapchain_->resize(extent);
        }
    }

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

    std::expected<void, std::string> create_allocator();
    void create_command_resources();
    VkCommandBuffer allocate_command_buffer(VkCommandPool pool);

    // Muss vor instance_ stehen: Die Debug-Schicht zählt bis zum Zerstören der Instanz.
    std::atomic<u32> validation_errors_{0};
    // Wird als letztes Mitglied zerstört, nach allen Objekten des Geräts.
    Instance instance_;
    PhysicalDevice physical_device_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;

    // Nur mit Fenster vorhanden.
    std::unique_ptr<Swapchain> swapchain_;

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
    if (auto result = instance_.init(desc, validation_errors_); !result) {
        return result;
    }
    if (desc.surface) {
        if (auto result = instance_.create_surface(*desc.surface); !result) {
            return result;
        }
    }

    auto physical_device = pick_physical_device(instance_.handle(), instance_.surface());
    if (!physical_device) {
        return std::unexpected(std::move(physical_device).error());
    }
    physical_device_ = *std::move(physical_device);

    const bool with_swapchain = instance_.surface() != VK_NULL_HANDLE;
    auto device = create_logical_device(physical_device_, with_swapchain);
    if (!device) {
        return std::unexpected(std::move(device).error());
    }
    device_ = *device;
    vkGetDeviceQueue(device_, physical_device_.queue_family, 0, &queue_);

    if (auto result = create_allocator(); !result) {
        return result;
    }
    create_command_resources();

    if (with_swapchain) {
        swapchain_ =
            std::make_unique<Swapchain>(physical_device_.handle, device_, instance_.surface(),
                                        desc.surface->extent, desc.vsync);
        if (auto result = swapchain_->rebuild(); !result) {
            return result;
        }
    }

    const Capabilities& caps = physical_device_.capabilities;
    log::info("Vulkan-Gerät: {} ({}), Vulkan {}", caps.device_name, to_string(caps.device_type),
              caps.api_version);
    return {};
}

std::expected<void, std::string> VulkanDevice::create_allocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    const VmaAllocatorCreateInfo allocator_info{
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = physical_device_.handle,
        .device = device_,
        .pVulkanFunctions = &functions,
        .instance = instance_.handle(),
        .vulkanApiVersion = required_api_version,
    };
    if (const VkResult result = vmaCreateAllocator(&allocator_info, &allocator_);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateAllocator", result));
    }
    return {};
}

VkCommandBuffer VulkanDevice::allocate_command_buffer(VkCommandPool pool) {
    const VkCommandBufferAllocateInfo info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device_, &info, &cmd), "vkAllocateCommandBuffers");
    return cmd;
}

void VulkanDevice::create_command_resources() {
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = physical_device_.queue_family,
    };
    const VkFenceCreateInfo signaled_fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                           .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    for (FrameData& frame : frames_) {
        check(vkCreateCommandPool(device_, &pool_info, nullptr, &frame.pool),
              "vkCreateCommandPool");
        frame.cmd = allocate_command_buffer(frame.pool);
        check(vkCreateFence(device_, &signaled_fence, nullptr, &frame.in_flight), "vkCreateFence");
        check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.image_available),
              "vkCreateSemaphore");
    }

    check(vkCreateCommandPool(device_, &pool_info, nullptr, &immediate_pool_),
          "vkCreateCommandPool");
    immediate_cmd_ = allocate_command_buffer(immediate_pool_);
    const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device_, &fence_info, nullptr, &immediate_fence_), "vkCreateFence");
}

VulkanDevice::~VulkanDevice() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    swapchain_.reset();
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
    // instance_ räumt sich danach als Mitglied selbst auf.
}

std::optional<Frame> VulkanDevice::begin_frame() {
    if (!swapchain_) {
        return std::nullopt;
    }
    if (swapchain_->needs_rebuild()) {
        // Blockierend, siehe TD-03.
        vkDeviceWaitIdle(device_);
        if (auto result = swapchain_->rebuild(); !result) {
            log::error("{}", result.error());
            return std::nullopt;
        }
        if (swapchain_->needs_rebuild()) {
            return std::nullopt;
        }
    }

    FrameData& frame = frames_[frame_index_];
    check(vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, std::numeric_limits<u64>::max()),
          "vkWaitForFences");

    const std::optional<u32> image_index = swapchain_->acquire(frame.image_available);
    if (!image_index) {
        return std::nullopt;
    }
    image_index_ = *image_index;

    // Erst nach erfolgreichem Acquire zurücksetzen, sonst wartet der nächste Frame ewig.
    check(vkResetFences(device_, 1, &frame.in_flight), "vkResetFences");
    check(vkResetCommandBuffer(frame.cmd, 0), "vkResetCommandBuffer");
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(frame.cmd, &begin_info), "vkBeginCommandBuffer");
    frame_commands_.reset(frame.cmd);

    VulkanTexture& backbuffer = swapchain_->backbuffer(image_index_);
    // Der alte Inhalt wird nicht gebraucht; das spart einen Übergang mit Kopie.
    backbuffer.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return Frame{frame_commands_, backbuffer};
}

void VulkanDevice::end_frame() {
    FrameData& frame = frames_[frame_index_];
    VulkanTexture& backbuffer = swapchain_->backbuffer(image_index_);
    transition(frame.cmd, backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    check(vkEndCommandBuffer(frame.cmd), "vkEndCommandBuffer");

    const VkSemaphoreSubmitInfo wait{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = frame.image_available,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkSemaphoreSubmitInfo signal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = swapchain_->render_finished(image_index_),
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
    swapchain_->present(queue_, image_index_);

    frame_index_ = (frame_index_ + 1) % frames_in_flight;
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
    // Blockierend, siehe TD-03.
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
    auto device = std::make_unique<VulkanDevice>();
    if (auto result = device->init(desc); !result) {
        return std::unexpected(result.error());
    }
    return device;
}

} // namespace ngine::rhi::vulkan
