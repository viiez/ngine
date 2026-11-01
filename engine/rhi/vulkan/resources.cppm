module;

#include "vk.hpp"

#include <memory>
#include <string>
#include <utility>

// GPU-Ressourcen des Vulkan-Backends: Texturen, Shader und Pipelines.
module ngine.rhi.vulkan:resources;

import ngine.core;
import ngine.rhi;

namespace ngine::rhi::vulkan {

class VulkanTexture final : public Texture {
public:
    // Ohne allocation gehört das Bild einem anderen Objekt (der Swapchain).
    VulkanTexture(VkDevice device, VmaAllocator allocator, VkImage image, VmaAllocation allocation,
                  VkImageView view, Extent2D extent, Format format)
        : device_{device}, allocator_{allocator}, image_{image}, allocation_{allocation},
          view_{view}, extent_{extent}, format_{format} {}

    ~VulkanTexture() override {
        vkDestroyImageView(device_, view_, nullptr);
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

// Erzeugt die Image-View eines 2D-Farbbilds.
[[nodiscard]] Result<VkImageView> create_color_view(VkDevice device, VkImage image,
                                                    VkFormat format);

[[nodiscard]] Result<std::unique_ptr<Texture>>
create_texture(VkDevice device, VmaAllocator allocator, const TextureDesc& desc);
[[nodiscard]] Result<std::unique_ptr<Shader>> create_shader(VkDevice device,
                                                            const ShaderDesc& desc);
[[nodiscard]] Result<std::unique_ptr<Pipeline>> create_pipeline(VkDevice device,
                                                                const GraphicsPipelineDesc& desc);

} // namespace ngine::rhi::vulkan
