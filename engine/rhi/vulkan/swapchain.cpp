module;

#include "vk.hpp"

#include <algorithm>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;
import :common;
import :resources;
import :swapchain;

namespace ngine::rhi::vulkan {

namespace {

VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const VkFormat preferred : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
        const auto match = std::ranges::find_if(formats, [&](const VkSurfaceFormatKHR& f) {
            return f.format == preferred && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (match != formats.end()) {
            return *match;
        }
    }
    return formats.front();
}

VkPresentModeKHR choose_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                     bool vsync) {
    // FIFO ist immer vorhanden und entspricht VSync.
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    u32 mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &mode_count, modes.data());
    for (const VkPresentModeKHR preferred :
         {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::ranges::find(modes, preferred) != modes.end()) {
            return preferred;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

} // namespace

std::expected<void, std::string> Swapchain::rebuild() {
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
        dirty_ = true;
        return {};
    }

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());
    if (formats.empty()) {
        return std::unexpected("Die Surface bietet keine Formate an");
    }
    const VkSurfaceFormatKHR surface_format = choose_surface_format(formats);

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
        .presentMode = choose_present_mode(physical_device_, surface_, vsync_),
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain,
    };
    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    if (const VkResult result =
            vkCreateSwapchainKHR(device_, &swapchain_info, nullptr, &new_swapchain);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateSwapchainKHR", result));
    }

    destroy_images();
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, old_swapchain, nullptr);
    }
    swapchain_ = new_swapchain;
    format_ = from_vk(surface_format.format);
    dirty_ = false;

    u32 count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, images.data());

    const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkImage image : images) {
        auto view = create_color_view(device_, image, surface_format.format);
        if (!view) {
            return std::unexpected(std::move(view).error());
        }
        // Die Bilder gehören der Swapchain und haben deshalb keine eigene Allokation.
        backbuffers_.push_back(
            std::make_unique<VulkanTexture>(device_, nullptr, image, nullptr, *view,
                                            Extent2D{extent.width, extent.height}, format_));

        VkSemaphore semaphore = VK_NULL_HANDLE;
        check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore),
              "vkCreateSemaphore");
        render_finished_.push_back(semaphore);
    }

    log::debug("Swapchain: {}x{}, {} Bilder", extent.width, extent.height, count);
    return {};
}

std::optional<u32> Swapchain::acquire(VkSemaphore image_available) {
    u32 image_index = 0;
    const VkResult acquired =
        vkAcquireNextImageKHR(device_, swapchain_, std::numeric_limits<u64>::max(), image_available,
                              VK_NULL_HANDLE, &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        dirty_ = true;
        return std::nullopt;
    }
    if (acquired == VK_SUBOPTIMAL_KHR) {
        // Das Bild ist noch nutzbar; neu aufgebaut wird vor dem nächsten Frame.
        dirty_ = true;
    } else {
        check(acquired, "vkAcquireNextImageKHR");
    }
    return image_index;
}

void Swapchain::present(VkQueue queue, u32 image_index) {
    const VkPresentInfoKHR present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_finished_[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &image_index,
    };
    const VkResult presented = vkQueuePresentKHR(queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        dirty_ = true;
    } else {
        check(presented, "vkQueuePresentKHR");
    }
}

void Swapchain::destroy_images() {
    backbuffers_.clear();
    for (VkSemaphore semaphore : render_finished_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_finished_.clear();
}

Swapchain::~Swapchain() {
    destroy_images();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }
}

} // namespace ngine::rhi::vulkan
