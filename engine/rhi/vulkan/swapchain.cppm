module;

#include "vk.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Swapchain mit ihren Bildern und den Semaphoren für die Präsentation.
module ngine.rhi.vulkan:swapchain;

import ngine.core;
import ngine.rhi;
import :resources;

namespace ngine::rhi::vulkan {

class Swapchain {
public:
    Swapchain(VkPhysicalDevice physical_device, VkDevice device, VkSurfaceKHR surface,
              Extent2D requested_extent, bool vsync)
        : physical_device_{physical_device}, device_{device}, surface_{surface},
          requested_extent_{requested_extent}, vsync_{vsync} {}
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Baut die Swapchain (neu) auf. Die GPU darf ihre Bilder nicht mehr benutzen.
    // Bei minimiertem Fenster bleibt needs_rebuild() wahr.
    [[nodiscard]] std::expected<void, std::string> rebuild();
    [[nodiscard]] bool needs_rebuild() const {
        return swapchain_ == VK_NULL_HANDLE || dirty_;
    }
    void resize(Extent2D extent) {
        requested_extent_ = extent;
        dirty_ = true;
    }

    // Holt das nächste Bild; image_available wird signalisiert, sobald es frei ist.
    // Liefert nichts, wenn die Swapchain veraltet ist.
    [[nodiscard]] std::optional<u32> acquire(VkSemaphore image_available);
    // Präsentiert das Bild, sobald render_finished(image_index) signalisiert ist.
    void present(VkQueue queue, u32 image_index);

    [[nodiscard]] VulkanTexture& backbuffer(u32 image_index) {
        return *backbuffers_[image_index];
    }
    // Ein Semaphor pro Swapchain-Bild, weil die Präsentation es erst später freigibt.
    [[nodiscard]] VkSemaphore render_finished(u32 image_index) const {
        return render_finished_[image_index];
    }
    [[nodiscard]] Format format() const {
        return format_;
    }

private:
    void destroy_images();

    VkPhysicalDevice physical_device_;
    VkDevice device_;
    VkSurfaceKHR surface_;
    Extent2D requested_extent_;
    bool vsync_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    Format format_ = Format::Unknown;
    bool dirty_ = false;
    std::vector<std::unique_ptr<VulkanTexture>> backbuffers_;
    std::vector<VkSemaphore> render_finished_;
};

} // namespace ngine::rhi::vulkan
