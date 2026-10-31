module;

#include "vk.hpp"

#include <optional>

// Aufzeichnen von Befehlen in einen VkCommandBuffer.
module ngine.rhi.vulkan:command_list;

import ngine.core;
import ngine.rhi;
import :resources;

namespace ngine::rhi::vulkan {

// Grober, aber sicherer Übergang zwischen zwei Layouts (TD-02). Feinere Barrieren übernimmt
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

// Zeichnet in einen fremden Command Buffer auf; Beginn, Ende und Abschicken übernimmt das Device.
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

} // namespace ngine::rhi::vulkan
