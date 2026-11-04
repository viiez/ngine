module;

#include "vk.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>

// GPU-Ressourcen des Vulkan-Backends: Texturen, Puffer, Sampler, Shader und Pipelines. Ihre
// Destruktoren geben die Vulkan-Objekte über die Löschliste frei, wenn die GPU fertig ist.
module ngine.rhi.vulkan:resources;

import ngine.core;
import ngine.rhi;
import :context;

namespace ngine::rhi::vulkan {

class VulkanTexture final : public Texture {
public:
    struct Parts {
        VkImage image = VK_NULL_HANDLE;
        // Ohne Allokation gehört das Bild einem anderen Objekt (der Swapchain).
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        u32 sampled_index = invalid_index;
        u32 storage_index = invalid_index;
        // Weitere Abgaben, die die Freigabe abwartet (Swapchain-Bilder, siehe Swapchain).
        u64 retire_delay = 0;
    };

    VulkanTexture(DeviceContext& context, const TextureDesc& desc, const Parts& parts)
        : context_{&context}, desc_{desc}, parts_{parts} {}

    ~VulkanTexture() override {
        context_->retire(
            [context = context_, parts = parts_] {
                vkDestroyImageView(context->device, parts.view, nullptr);
                if (parts.allocation != nullptr) {
                    vmaDestroyImage(context->allocator, parts.image, parts.allocation);
                }
                context->bindless.release(BindlessKind::SampledImage, parts.sampled_index);
                context->bindless.release(BindlessKind::StorageImage, parts.storage_index);
            },
            parts_.retire_delay);
    }

    [[nodiscard]] Extent2D extent() const override {
        return desc_.extent;
    }
    [[nodiscard]] Format format() const override {
        return desc_.format;
    }
    [[nodiscard]] TextureUsage usage() const override {
        return desc_.usage;
    }
    [[nodiscard]] u32 mip_levels() const override {
        return desc_.mip_levels;
    }
    [[nodiscard]] u32 sampled_index() const override {
        return parts_.sampled_index;
    }
    [[nodiscard]] u32 storage_index() const override {
        return parts_.storage_index;
    }

    [[nodiscard]] VkImage image() const {
        return parts_.image;
    }
    [[nodiscard]] VkImageView view() const {
        return parts_.view;
    }
    [[nodiscard]] VkImageAspectFlags aspect() const {
        return is_depth(desc_.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    }

private:
    DeviceContext* context_;
    TextureDesc desc_;
    Parts parts_;
};

class VulkanBuffer final : public Buffer {
public:
    VulkanBuffer(DeviceContext& context, VkBuffer buffer, VmaAllocation allocation, void* mapped,
                 const BufferDesc& desc, u64 address)
        : context_{&context}, buffer_{buffer}, allocation_{allocation}, mapped_{mapped},
          desc_{desc}, address_{address} {}

    ~VulkanBuffer() override {
        context_->retire(
            [allocator = context_->allocator, buffer = buffer_, allocation = allocation_] {
                vmaDestroyBuffer(allocator, buffer, allocation);
            });
    }

    [[nodiscard]] u64 size() const override {
        return desc_.size;
    }
    [[nodiscard]] u64 device_address() const override {
        return address_;
    }
    [[nodiscard]] MemoryUsage memory() const override {
        return desc_.memory;
    }
    void write(u64 offset, std::span<const std::byte> data) override;

    [[nodiscard]] VkBuffer handle() const {
        return buffer_;
    }

private:
    DeviceContext* context_;
    VkBuffer buffer_;
    VmaAllocation allocation_;
    void* mapped_;
    BufferDesc desc_;
    u64 address_;
};

class VulkanSampler final : public Sampler {
public:
    VulkanSampler(DeviceContext& context, VkSampler sampler, u32 index)
        : context_{&context}, sampler_{sampler}, index_{index} {}

    ~VulkanSampler() override {
        context_->retire([context = context_, sampler = sampler_, index = index_] {
            vkDestroySampler(context->device, sampler, nullptr);
            context->bindless.release(BindlessKind::Sampler, index);
        });
    }

    [[nodiscard]] u32 index() const override {
        return index_;
    }

private:
    DeviceContext* context_;
    VkSampler sampler_;
    u32 index_;
};

// Shader-Module braucht nur das Erzeugen von Pipelines; sie werden sofort zerstört.
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

// Das Layout gehört dem bindlosen Satz und ist für alle Pipelines gleich.
class VulkanPipeline final : public Pipeline {
public:
    VulkanPipeline(DeviceContext& context, VkPipeline pipeline, VkPipelineBindPoint bind_point)
        : context_{&context}, pipeline_{pipeline}, bind_point_{bind_point} {}

    ~VulkanPipeline() override {
        context_->retire([device = context_->device, pipeline = pipeline_] {
            vkDestroyPipeline(device, pipeline, nullptr);
        });
    }

    [[nodiscard]] VkPipeline handle() const {
        return pipeline_;
    }
    [[nodiscard]] VkPipelineBindPoint bind_point() const {
        return bind_point_;
    }

private:
    DeviceContext* context_;
    VkPipeline pipeline_;
    VkPipelineBindPoint bind_point_;
};

// Erzeugt die Image-View eines 2D-Bilds über alle Mip-Stufen.
[[nodiscard]] Result<VkImageView> create_view(VkDevice device, VkImage image, VkFormat format,
                                              VkImageAspectFlags aspect, u32 mip_levels = 1);

[[nodiscard]] Result<std::unique_ptr<Texture>> create_texture(DeviceContext& context,
                                                              const TextureDesc& desc);
[[nodiscard]] Result<std::unique_ptr<Buffer>> create_buffer(DeviceContext& context,
                                                            const BufferDesc& desc);
[[nodiscard]] Result<std::unique_ptr<Sampler>> create_sampler(DeviceContext& context,
                                                              const SamplerDesc& desc);
[[nodiscard]] Result<std::unique_ptr<Shader>> create_shader(VkDevice device,
                                                            const ShaderDesc& desc);
[[nodiscard]] Result<std::unique_ptr<Pipeline>> create_pipeline(DeviceContext& context,
                                                                const GraphicsPipelineDesc& desc);
[[nodiscard]] Result<std::unique_ptr<Pipeline>>
create_compute_pipeline(DeviceContext& context, const ComputePipelineDesc& desc);

} // namespace ngine::rhi::vulkan
