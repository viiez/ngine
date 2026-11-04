module;

#include "vk.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;
import :common;
import :context;
import :resources;

namespace ngine::rhi::vulkan {

Result<VkImageView> create_view(VkDevice device, VkImage image, VkFormat format,
                                VkImageAspectFlags aspect, u32 mip_levels) {
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {aspect, 0, mip_levels, 0, 1},
    };
    VkImageView view = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateImageView(device, &view_info, nullptr, &view);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateImageView", result));
    }
    return view;
}

Result<std::unique_ptr<Texture>> create_texture(DeviceContext& context, const TextureDesc& desc) {
    const VkFormat format = to_vk(desc.format);
    if (format == VK_FORMAT_UNDEFINED || desc.extent.width == 0 || desc.extent.height == 0) {
        return std::unexpected("Ungültige Textur-Beschreibung");
    }
    const bool depth = is_depth(desc.format);
    if (desc.mip_levels == 0 || desc.mip_levels > full_mip_count(desc.extent)) {
        return std::unexpected("Ungültige Zahl von Mip-Stufen");
    }
    const bool mipmapped = desc.mip_levels > 1;
    if (mipmapped &&
        (has(desc.usage, TextureUsage::RenderTarget) || has(desc.usage, TextureUsage::Storage))) {
        return std::unexpected("Mip-Stufen nur für Texturen ohne RenderTarget und Storage");
    }

    VkImageUsageFlags usage = 0;
    if (has(desc.usage, TextureUsage::RenderTarget)) {
        usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                       : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (has(desc.usage, TextureUsage::Sampled)) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (has(desc.usage, TextureUsage::Storage)) {
        // sRGB- und Tiefenformate können GPUs meist nicht als Storage-Bild schreiben.
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(context.physical_device, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
            return std::unexpected("Das Format taugt auf dieser GPU nicht als Storage-Textur");
        }
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (has(desc.usage, TextureUsage::TransferSrc)) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (has(desc.usage, TextureUsage::TransferDst)) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (usage == 0) {
        return std::unexpected("Textur ohne Verwendung");
    }
    if (mipmapped) {
        // upload() rechnet die Stufen per Blit von Stufe zu Stufe.
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(context.physical_device, format, &properties);
        constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                                VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & needed) != needed) {
            return std::unexpected("Für dieses Format kann die GPU keine Mip-Stufen erzeugen");
        }
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {desc.extent.width, desc.extent.height, 1},
        .mipLevels = desc.mip_levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO};
    VulkanTexture::Parts parts;
    if (const VkResult result = vmaCreateImage(context.allocator, &image_info, &allocation_info,
                                               &parts.image, &parts.allocation, nullptr);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateImage", result));
    }

    auto view =
        create_view(context.device, parts.image, format,
                    depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, desc.mip_levels);
    if (!view) {
        vmaDestroyImage(context.allocator, parts.image, parts.allocation);
        return std::unexpected(std::move(view).error());
    }
    parts.view = *view;

    const auto fail = [&](std::string message) -> Result<std::unique_ptr<Texture>> {
        context.bindless.release(BindlessKind::SampledImage, parts.sampled_index);
        vkDestroyImageView(context.device, parts.view, nullptr);
        vmaDestroyImage(context.allocator, parts.image, parts.allocation);
        return std::unexpected(std::move(message));
    };
    if (has(desc.usage, TextureUsage::Sampled)) {
        parts.sampled_index = context.bindless.allocate(BindlessKind::SampledImage);
        if (parts.sampled_index == invalid_index) {
            return fail("Kein freier Platz für Texturen im bindlosen Satz");
        }
        context.bindless.write_image(BindlessKind::SampledImage, parts.sampled_index, parts.view);
    }
    if (has(desc.usage, TextureUsage::Storage)) {
        parts.storage_index = context.bindless.allocate(BindlessKind::StorageImage);
        if (parts.storage_index == invalid_index) {
            return fail("Kein freier Platz für Storage-Texturen im bindlosen Satz");
        }
        context.bindless.write_image(BindlessKind::StorageImage, parts.storage_index, parts.view);
    }
    return std::make_unique<VulkanTexture>(context, desc, parts);
}

void VulkanBuffer::write(u64 offset, std::span<const std::byte> data) {
    verify(desc_.memory == MemoryUsage::CpuWrite,
           "write() nur für Puffer mit MemoryUsage::CpuWrite, sonst Device::upload()");
    verify(offset <= desc_.size && data.size() <= desc_.size - offset,
           "Schreiben über das Ende des Puffers");
    if (data.empty()) {
        return;
    }
    std::memcpy(static_cast<std::byte*>(mapped_) + offset, data.data(), data.size());
    // Auf Speicher ohne HOST_COHERENT muss die Änderung ausdrücklich sichtbar werden.
    check(vmaFlushAllocation(context_->allocator, allocation_, offset, data.size()),
          "vmaFlushAllocation");
}

Result<std::unique_ptr<Buffer>> create_buffer(DeviceContext& context, const BufferDesc& desc) {
    if (desc.size == 0) {
        return std::unexpected("Puffer mit Größe 0");
    }
    // Jeder Puffer kann Ziel einer Kopie sein (upload()); das kostet nichts.
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (has(desc.usage, BufferUsage::Index)) {
        usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO};
    if (desc.memory == MemoryUsage::CpuWrite) {
        // Sequentielles Schreiben von der CPU: Mit Resizable BAR landet der Puffer im
        // GPU-Speicher, sonst im Systemspeicher, den die GPU über PCIe liest.
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo allocated{};
    if (const VkResult result = vmaCreateBuffer(context.allocator, &buffer_info, &allocation_info,
                                                &buffer, &allocation, &allocated);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateBuffer", result));
    }
    const VkBufferDeviceAddressInfo address_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    const VkDeviceAddress address = vkGetBufferDeviceAddress(context.device, &address_info);
    return std::make_unique<VulkanBuffer>(context, buffer, allocation, allocated.pMappedData, desc,
                                          address);
}

Result<std::unique_ptr<Sampler>> create_sampler(DeviceContext& context, const SamplerDesc& desc) {
    const auto filter = [](Filter value) {
        return value == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    };
    const auto address = [](AddressMode mode) {
        switch (mode) {
        case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    };
    const f32 anisotropy = std::clamp(desc.anisotropy, 1.0f, context.max_anisotropy);
    const VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = filter(desc.mag_filter),
        .minFilter = filter(desc.min_filter),
        .mipmapMode = desc.mip_filter == Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                        : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = address(desc.address_u),
        .addressModeV = address(desc.address_v),
        .addressModeW = address(desc.address_u),
        .anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = anisotropy,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateSampler(context.device, &sampler_info, nullptr, &sampler);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateSampler", result));
    }
    const u32 index = context.bindless.allocate(BindlessKind::Sampler);
    if (index == invalid_index) {
        vkDestroySampler(context.device, sampler, nullptr);
        return std::unexpected("Kein freier Platz für Sampler im bindlosen Satz");
    }
    context.bindless.write_sampler(index, sampler);
    return std::make_unique<VulkanSampler>(context, sampler, index);
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

Result<std::unique_ptr<Pipeline>> create_pipeline(DeviceContext& context,
                                                  const GraphicsPipelineDesc& desc) {
    const VkFormat color_format = to_vk(desc.color_format);
    const bool with_color = color_format != VK_FORMAT_UNDEFINED;
    if (desc.vertex == nullptr || (with_color && desc.fragment == nullptr)) {
        return std::unexpected("Pipeline braucht Vertex- und Fragment-Shader");
    }
    if (!with_color && desc.depth_format == Format::Unknown) {
        return std::unexpected("Pipeline braucht ein Farb- oder Tiefenformat");
    }

    const auto& vertex = static_cast<const VulkanShader&>(*desc.vertex);
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex.module(),
            .pName = vertex.entry_point().c_str(),
        },
        {},
    };
    u32 stage_count = 1;
    if (desc.fragment != nullptr) {
        const auto& fragment = static_cast<const VulkanShader&>(*desc.fragment);
        stages[stage_count++] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment.module(),
            .pName = fragment.entry_point().c_str(),
        };
    }

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
    const VkFormat depth_format = to_vk(desc.depth_format);
    if (desc.depth_format != Format::Unknown && !is_depth(desc.depth_format)) {
        return std::unexpected("Das Tiefenformat der Pipeline ist kein Tiefenformat");
    }
    const bool with_depth = depth_format != VK_FORMAT_UNDEFINED;

    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = desc.cull_mode == CullMode::Back ? VkCullModeFlags{VK_CULL_MODE_BACK_BIT}
                                                     : VkCullModeFlags{VK_CULL_MODE_NONE},
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable =
            desc.depth_bias_constant != 0.0f || desc.depth_bias_slope != 0.0f ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = desc.depth_bias_constant,
        .depthBiasSlopeFactor = desc.depth_bias_slope,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkCompareOp compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    switch (desc.depth_compare) {
    case CompareOp::LessOrEqual: compare = VK_COMPARE_OP_LESS_OR_EQUAL; break;
    case CompareOp::Greater: compare = VK_COMPARE_OP_GREATER; break;
    case CompareOp::GreaterOrEqual: compare = VK_COMPARE_OP_GREATER_OR_EQUAL; break;
    }
    const VkPipelineDepthStencilStateCreateInfo depth_stencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = with_depth ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = with_depth && desc.depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = compare,
    };
    const bool alpha = desc.blend == BlendMode::Alpha;
    const VkPipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = alpha ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = with_color ? 1u : 0u,
        .pAttachments = with_color ? &blend_attachment : nullptr,
    };
    const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = with_color ? 1u : 0u,
        .pColorAttachmentFormats = with_color ? &color_format : nullptr,
        .depthAttachmentFormat = depth_format,
    };

    const VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = stage_count,
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = context.bindless.pipeline_layout(),
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1,
                                                          &pipeline_info, nullptr, &pipeline);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateGraphicsPipelines", result));
    }
    return std::make_unique<VulkanPipeline>(context, pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS);
}

Result<std::unique_ptr<Pipeline>> create_compute_pipeline(DeviceContext& context,
                                                          const ComputePipelineDesc& desc) {
    if (desc.compute == nullptr) {
        return std::unexpected("Compute-Pipeline braucht einen Shader");
    }
    const auto& shader = static_cast<const VulkanShader&>(*desc.compute);
    if (shader.stage() != ShaderStage::Compute) {
        return std::unexpected("Der Shader der Compute-Pipeline ist kein Compute-Shader");
    }
    const VkComputePipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = shader.module(),
                .pName = shader.entry_point().c_str(),
            },
        .layout = context.bindless.pipeline_layout(),
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1,
                                                         &pipeline_info, nullptr, &pipeline);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateComputePipelines", result));
    }
    return std::make_unique<VulkanPipeline>(context, pipeline, VK_PIPELINE_BIND_POINT_COMPUTE);
}

} // namespace ngine::rhi::vulkan
