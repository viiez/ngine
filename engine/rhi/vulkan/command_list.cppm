module;

#include "vk.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// Aufzeichnen von Befehlen in einen VkCommandBuffer.
module ngine.rhi.vulkan:command_list;

import ngine.core;
import ngine.rhi;
import :common;
import :context;
import :resources;

namespace ngine::rhi::vulkan {

[[nodiscard]] VkImageMemoryBarrier2 image_barrier(const VulkanTexture& texture,
                                                  ResourceState before, ResourceState after) {
    verify(after != ResourceState::Undefined, "Barriere nach Undefined");
    const StateInfo src = state_info(before);
    const StateInfo dst = state_info(after);
    return VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src.stages,
        .srcAccessMask = src.access,
        .dstStageMask = dst.stages,
        .dstAccessMask = dst.access,
        .oldLayout = src.layout,
        .newLayout = dst.layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture.image(),
        .subresourceRange = {texture.aspect(), 0, VK_REMAINING_MIP_LEVELS, 0, 1},
    };
}

// Globale Speicherbarriere: alles davor (alle Stufen, alle Schreibzugriffe) vor allem danach.
void full_barrier(VkCommandBuffer cmd) {
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// Zeichnet in einen fremden Command Buffer auf; Beginn, Ende und Abschicken übernimmt das Device.
class VulkanCommandList final : public CommandList {
public:
    explicit VulkanCommandList(const BindlessHeap& bindless) : bindless_{&bindless} {}

    // Übernimmt einen Command Buffer im Aufzeichnungszustand und bindet den bindlosen Satz für
    // Grafik und Compute; er bleibt über alle Pipeline-Wechsel gebunden, weil alle Pipelines
    // dasselbe Layout haben.
    void reset(VkCommandBuffer cmd) {
        cmd_ = cmd;
        bound_ = false;
        const VkDescriptorSet set = bindless_->set();
        for (const VkPipelineBindPoint point :
             {VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE}) {
            vkCmdBindDescriptorSets(cmd_, point, bindless_->pipeline_layout(), 0, 1, &set, 0,
                                    nullptr);
        }
    }
    [[nodiscard]] VkCommandBuffer handle() const {
        return cmd_;
    }

    void barrier(std::span<const TextureBarrier> textures,
                 std::span<const BufferBarrier> buffers) override {
        image_barriers_.clear();
        buffer_barriers_.clear();
        for (const TextureBarrier& b : textures) {
            image_barriers_.push_back(
                image_barrier(static_cast<const VulkanTexture&>(*b.texture), b.before, b.after));
        }
        for (const BufferBarrier& b : buffers) {
            verify(b.after != ResourceState::Undefined, "Barriere nach Undefined");
            const StateInfo src = state_info(b.before);
            const StateInfo dst = state_info(b.after);
            buffer_barriers_.push_back(VkBufferMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = src.stages,
                .srcAccessMask = src.access,
                .dstStageMask = dst.stages,
                .dstAccessMask = dst.access,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = static_cast<const VulkanBuffer&>(*b.buffer).handle(),
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            });
        }
        if (image_barriers_.empty() && buffer_barriers_.empty()) {
            return;
        }
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = static_cast<u32>(buffer_barriers_.size()),
            .pBufferMemoryBarriers = buffer_barriers_.data(),
            .imageMemoryBarrierCount = static_cast<u32>(image_barriers_.size()),
            .pImageMemoryBarriers = image_barriers_.data(),
        };
        vkCmdPipelineBarrier2(cmd_, &dependency);
    }

    void begin_rendering(const RenderingDesc& desc) override {
        verify(desc.color != nullptr || desc.depth != nullptr,
               "begin_rendering braucht ein Farbziel oder einen Tiefenpuffer");
        // Die Größe kommt vom Farbziel, ohne eines vom Tiefenpuffer.
        const auto& texture =
            static_cast<const VulkanTexture&>(desc.color != nullptr ? *desc.color : *desc.depth);

        const Color color = desc.clear_color.value_or(Color{});
        const VkRenderingAttachmentInfo attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = desc.color != nullptr ? texture.view() : VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = desc.clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = {.float32 = {color.r, color.g, color.b, color.a}}},
        };

        VkRenderingAttachmentInfo depth_attachment{.sType =
                                                       VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (desc.depth != nullptr) {
            const auto& depth = static_cast<const VulkanTexture&>(*desc.depth);
            depth_attachment.imageView = depth.view();
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp =
                desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth_attachment.clearValue.depthStencil = {desc.clear_depth.value_or(1.0f), 0};
        }

        const VkExtent2D extent{texture.extent().width, texture.extent().height};
        const VkRenderingInfo rendering{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, extent},
            .layerCount = 1,
            .colorAttachmentCount = desc.color != nullptr ? 1u : 0u,
            .pColorAttachments = desc.color != nullptr ? &attachment : nullptr,
            .pDepthAttachment = desc.depth != nullptr ? &depth_attachment : nullptr,
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

    void set_scissor(const Rect& rect) override {
        verify(rect.x >= 0 && rect.y >= 0, "set_scissor: negativer Ursprung");
        // Der Scissor gilt im Framebuffer (Ursprung oben links); die Spiegelung des Viewports
        // betrifft ihn nicht.
        const VkRect2D scissor{{rect.x, rect.y}, {rect.width, rect.height}};
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
    }

    void bind_pipeline(const Pipeline& pipeline) override {
        const auto& vulkan_pipeline = static_cast<const VulkanPipeline&>(pipeline);
        vkCmdBindPipeline(cmd_, vulkan_pipeline.bind_point(), vulkan_pipeline.handle());
        bound_ = true;
    }

    void push_constants(std::span<const std::byte> data) override {
        verify(bound_, "push_constants ohne gebundene Pipeline");
        verify(data.size() <= max_push_constant_size, "Zu viele Push Constants");
        vkCmdPushConstants(cmd_, bindless_->pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                           static_cast<u32>(data.size()), data.data());
    }

    void bind_index_buffer(const Buffer& buffer) override {
        vkCmdBindIndexBuffer(cmd_, static_cast<const VulkanBuffer&>(buffer).handle(), 0,
                             VK_INDEX_TYPE_UINT32);
    }

    void draw(u32 vertex_count, u32 instance_count) override {
        vkCmdDraw(cmd_, vertex_count, instance_count, 0, 0);
    }

    void draw_indexed(u32 index_count, u32 instance_count, u32 first_index) override {
        vkCmdDrawIndexed(cmd_, index_count, instance_count, first_index, 0, 0);
    }

    void dispatch(u32 groups_x, u32 groups_y, u32 groups_z) override {
        vkCmdDispatch(cmd_, groups_x, groups_y, groups_z);
    }

    void write_timestamp(u32 index) override {
        if (timestamps_ == VK_NULL_HANDLE || index >= max_frame_timestamps ||
            index > timestamp_count_) {
            return;
        }
        vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps_,
                             timestamp_base_ + index);
        timestamp_count_ = std::max(timestamp_count_, index + 1);
    }

    // Zeitstempel dieses Command Buffers landen ab base im Pool; ohne Pool keine.
    void set_timestamps(VkQueryPool pool, u32 base) {
        timestamps_ = pool;
        timestamp_base_ = base;
        timestamp_count_ = 0;
    }
    [[nodiscard]] u32 timestamp_count() const {
        return timestamp_count_;
    }

private:
    const BindlessHeap* bindless_;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool bound_ = false;
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
    u32 timestamp_base_ = 0;
    u32 timestamp_count_ = 0;
    std::vector<VkImageMemoryBarrier2> image_barriers_;
    std::vector<VkBufferMemoryBarrier2> buffer_barriers_;
};

} // namespace ngine::rhi::vulkan
