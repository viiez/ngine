module;

#include "vk.hpp"

#include <array>
#include <expected>
#include <memory>
#include <utility>

module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;
import :common;
import :resources;

namespace ngine::rhi::vulkan {

Result<VkImageView> create_color_view(VkDevice device, VkImage image, VkFormat format) {
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkImageView view = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateImageView(device, &view_info, nullptr, &view);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateImageView", result));
    }
    return view;
}

Result<std::unique_ptr<Texture>> create_texture(VkDevice device, VmaAllocator allocator,
                                                const TextureDesc& desc) {
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
            vmaCreateImage(allocator, &image_info, &allocation_info, &image, &allocation, nullptr);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateImage", result));
    }

    auto view = create_color_view(device, image, format);
    if (!view) {
        vmaDestroyImage(allocator, image, allocation);
        return std::unexpected(std::move(view).error());
    }
    return std::make_unique<VulkanTexture>(device, allocator, image, allocation, *view, desc.extent,
                                           desc.format);
}

Result<std::unique_ptr<Shader>> create_shader(VkDevice device, const ShaderDesc& desc) {
    if (desc.spirv.empty()) {
        return std::unexpected("Shader ohne SPIR-V-Code");
    }
    const VkShaderModuleCreateInfo module_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = desc.spirv.size_bytes(),
        .pCode = desc.spirv.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateShaderModule(device, &module_info, nullptr, &module);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateShaderModule", result));
    }
    return std::make_unique<VulkanShader>(device, module, desc.stage, desc.entry_point);
}

Result<std::unique_ptr<Pipeline>> create_pipeline(VkDevice device,
                                                  const GraphicsPipelineDesc& desc) {
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
    if (const VkResult result = vkCreatePipelineLayout(device, &layout_info, nullptr, &layout);
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
    if (const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                          nullptr, &pipeline);
        result != VK_SUCCESS) {
        vkDestroyPipelineLayout(device, layout, nullptr);
        return std::unexpected(failed("vkCreateGraphicsPipelines", result));
    }
    return std::make_unique<VulkanPipeline>(device, layout, pipeline);
}

} // namespace ngine::rhi::vulkan
