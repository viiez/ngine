module;

#include "vk.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

// Hochladen in GPU-Speicher über Staging-Puffer (TD-11). Kopien sammeln sich in einem eigenen
// Command Buffer, den das Device vor der nächsten Abgabe einreiht. Niemand wartet auf die GPU:
// Staging-Speicher und Command Buffer kommen über die Timeline zurück. Seit R2.4 (TD-17) gibt es
// zwei Uploader: einen für die Grafik-Queue und, wo die GPU eine hat, einen für die
// Transfer-Queue (upload_async()), der nur kopieren kann.
module ngine.rhi.vulkan:upload;

import ngine.core;
import ngine.rhi;
import :common;
import :context;
import :resources;
import :command_list;

namespace ngine::rhi::vulkan {

class Uploader {
public:
    // Größe eines Staging-Blocks; größere Kopien bekommen einen eigenen Block.
    static constexpr u64 chunk_size = u64{16} << 20U;

    // transfer_only: Die Queue kann nur kopieren; Barrieren nennen dann keine Shader-Stufen.
    void init(DeviceContext& context, u32 queue_family, bool transfer_only = false) {
        context_ = &context;
        transfer_only_ = transfer_only;
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue_family,
        };
        check(vkCreateCommandPool(context.device, &pool_info, nullptr, &pool_),
              "vkCreateCommandPool");
    }

    // Nur nach vkDeviceWaitIdle.
    void destroy() {
        if (context_ == nullptr) {
            return;
        }
        for (auto* list : {&open_, &in_flight_, &free_}) {
            for (const Chunk& chunk : *list) {
                vmaDestroyBuffer(context_->allocator, chunk.buffer, chunk.allocation);
            }
            list->clear();
        }
        vkDestroyCommandPool(context_->device, pool_, nullptr);
        context_ = nullptr;
    }

    void upload(const VulkanBuffer& buffer, u64 offset, std::span<const std::byte> data) {
        verify(offset <= buffer.size() && data.size() <= buffer.size() - offset,
               "upload() über das Ende des Puffers");
        if (data.empty()) {
            return;
        }
        const Slice slice = stage(data);
        const VkBufferCopy region{
            .srcOffset = slice.offset, .dstOffset = offset, .size = data.size()};
        vkCmdCopyBuffer(batch(), slice.buffer, buffer.handle(), 1, &region);
    }

    void upload(const VulkanTexture& texture, std::span<const std::byte> data) {
        verify(has(texture.usage(), TextureUsage::TransferDst) &&
                   has(texture.usage(), TextureUsage::Sampled),
               "upload() braucht eine Textur mit TransferDst und Sampled");
        const Extent2D extent = texture.extent();
        verify(data.size() == u64{extent.width} * extent.height * bytes_per_pixel(texture.format()),
               "upload(): Datenmenge passt nicht zur Textur");
        const Slice slice = stage(data);
        const VkCommandBuffer cmd = batch();

        const auto transition = [&](ResourceState before, ResourceState after) {
            const VkImageMemoryBarrier2 barrier = image_barrier(texture, before, after);
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &barrier,
            };
            vkCmdPipelineBarrier2(cmd, &dependency);
        };
        transition(ResourceState::Undefined, ResourceState::TransferDst);
        const VkBufferImageCopy region{
            .bufferOffset = slice.offset,
            .imageSubresource = {texture.aspect(), 0, 0, 1},
            .imageExtent = {extent.width, extent.height, 1},
        };
        vkCmdCopyBufferToImage(cmd, slice.buffer, texture.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (texture.mip_levels() == 1) {
            transition(ResourceState::TransferDst, ResourceState::ShaderRead);
            return;
        }
        generate_mips(cmd, texture);
    }

    // Alle Mip-Stufen einer Textur aus data (Stufe 0 zuerst, Zeilen ohne Abstand); danach ist die
    // Textur im Zustand ShaderRead. Die GPU rechnet nichts nach, deshalb geht es auch auf einer
    // Queue, die nur kopieren kann.
    void upload_levels(const VulkanTexture& texture, std::span<const std::byte> data) {
        verify(has(texture.usage(), TextureUsage::TransferDst) &&
                   has(texture.usage(), TextureUsage::Sampled),
               "upload_async() braucht eine Textur mit TransferDst und Sampled");
        const Extent2D extent = texture.extent();
        const u32 pixel = bytes_per_pixel(texture.format());
        const u32 levels = texture.mip_levels();
        verify(data.size() == mip_chain_size(extent, levels, pixel),
               "upload_async(): Datenmenge passt nicht zu den Mip-Stufen der Textur");
        const Slice slice = stage(data);
        const VkCommandBuffer cmd = batch();

        VkImageMemoryBarrier2 barrier =
            image_barrier(texture, ResourceState::Undefined, ResourceState::TransferDst);
        VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &dependency);

        std::vector<VkBufferImageCopy> regions;
        u64 offset = slice.offset;
        u32 width = extent.width;
        u32 height = extent.height;
        for (u32 level = 0; level < levels; ++level) {
            regions.push_back({
                .bufferOffset = offset,
                .imageSubresource = {texture.aspect(), level, 0, 1},
                .imageExtent = {width, height, 1},
            });
            offset += u64{width} * height * pixel;
            width = std::max(width / 2, 1u);
            height = std::max(height / 2, 1u);
        }
        vkCmdCopyBufferToImage(cmd, slice.buffer, texture.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<u32>(regions.size()), regions.data());

        barrier = image_barrier(texture, ResourceState::TransferDst, ResourceState::ShaderRead);
        if (transfer_only_) {
            // Die Shader-Stufen gibt es auf dieser Queue nicht; die Grafik-Queue wartet über die
            // Timeline der Transfer-Queue.
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = VK_ACCESS_2_NONE;
        }
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    [[nodiscard]] bool has_batch() const noexcept {
        return current_ != VK_NULL_HANDLE;
    }

    // Schließt die gesammelten Kopien ab. Liefert den Command Buffer für die Abgabe mit dem Wert
    // submission, oder VK_NULL_HANDLE, wenn nichts ansteht.
    [[nodiscard]] VkCommandBuffer finish(u64 submission) {
        if (current_ == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }
        // Die Kopien vor aller Arbeit, die danach abgegeben wird.
        full_barrier(current_);
        check(vkEndCommandBuffer(current_), "vkEndCommandBuffer");
        for (Chunk& chunk : open_) {
            check(vmaFlushAllocation(context_->allocator, chunk.allocation, 0, chunk.used),
                  "vmaFlushAllocation");
            chunk.retire = submission;
            in_flight_.push_back(chunk);
        }
        open_.clear();
        batches_.push_back({current_, submission});
        const VkCommandBuffer cmd = current_;
        current_ = VK_NULL_HANDLE;
        return cmd;
    }

    // Gibt Staging-Speicher und Command Buffer frei, deren Abgabe fertig ist.
    void collect(u64 completed) {
        std::erase_if(in_flight_, [&](Chunk& chunk) {
            if (chunk.retire > completed) {
                return false;
            }
            if (chunk.size > chunk_size) {
                vmaDestroyBuffer(context_->allocator, chunk.buffer, chunk.allocation);
            } else {
                chunk.used = 0;
                free_.push_back(chunk);
            }
            return true;
        });
        std::erase_if(batches_, [&](const Batch& b) {
            if (b.retire > completed) {
                return false;
            }
            free_commands_.push_back(b.cmd);
            return true;
        });
    }

    // Bytes, die gerade im Staging-Speicher stecken (für Tests und Statistik).
    [[nodiscard]] usize pending_chunks() const {
        return open_.size() + in_flight_.size();
    }

private:
    // Rechnet die Stufen 1 bis n-1 aus Stufe 0, jede per linearem Blit aus der vorigen. Vorher
    // sind alle Stufen in TransferDst, danach in ShaderRead.
    static void generate_mips(VkCommandBuffer cmd, const VulkanTexture& texture) {
        const auto level_barrier = [&](u32 level, ResourceState before, ResourceState after) {
            VkImageMemoryBarrier2 barrier = image_barrier(texture, before, after);
            barrier.subresourceRange.baseMipLevel = level;
            barrier.subresourceRange.levelCount = 1;
            return barrier;
        };
        const auto submit = [&](std::span<const VkImageMemoryBarrier2> barriers) {
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                .pImageMemoryBarriers = barriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dependency);
        };

        const u32 levels = texture.mip_levels();
        i32 width = static_cast<i32>(texture.extent().width);
        i32 height = static_cast<i32>(texture.extent().height);
        for (u32 level = 1; level < levels; ++level) {
            const VkImageMemoryBarrier2 source =
                level_barrier(level - 1, ResourceState::TransferDst, ResourceState::TransferSrc);
            submit(std::span{&source, 1});
            const i32 next_width = std::max(width / 2, 1);
            const i32 next_height = std::max(height / 2, 1);
            const VkImageBlit2 region{
                .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
                .srcSubresource = {texture.aspect(), level - 1, 0, 1},
                .srcOffsets = {{0, 0, 0}, {width, height, 1}},
                .dstSubresource = {texture.aspect(), level, 0, 1},
                .dstOffsets = {{0, 0, 0}, {next_width, next_height, 1}},
            };
            const VkBlitImageInfo2 blit{
                .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
                .srcImage = texture.image(),
                .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .dstImage = texture.image(),
                .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .regionCount = 1,
                .pRegions = &region,
                .filter = VK_FILTER_LINEAR,
            };
            vkCmdBlitImage2(cmd, &blit);
            width = next_width;
            height = next_height;
        }
        const std::array final{
            [&] {
                VkImageMemoryBarrier2 barrier =
                    image_barrier(texture, ResourceState::TransferSrc, ResourceState::ShaderRead);
                barrier.subresourceRange.levelCount = levels - 1;
                return barrier;
            }(),
            level_barrier(levels - 1, ResourceState::TransferDst, ResourceState::ShaderRead),
        };
        submit(final);
    }

    struct Chunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        std::byte* mapped = nullptr;
        u64 size = 0;
        u64 used = 0;
        u64 retire = 0;
    };
    struct Slice {
        VkBuffer buffer;
        u64 offset;
    };
    struct Batch {
        VkCommandBuffer cmd;
        u64 retire;
    };

    Slice stage(std::span<const std::byte> data) {
        // 16 Bytes Ausrichtung genügen für jedes Texelformat und jede Pufferkopie.
        const u64 size = (data.size() + 15) & ~u64{15};
        Chunk* chunk = open_.empty() ? nullptr : &open_.back();
        if (chunk == nullptr || chunk->size - chunk->used < size) {
            chunk = &open_.emplace_back(take_chunk(size));
        }
        const Slice slice{chunk->buffer, chunk->used};
        std::memcpy(chunk->mapped + chunk->used, data.data(), data.size());
        chunk->used += size;
        return slice;
    }

    Chunk take_chunk(u64 size) {
        if (size <= chunk_size && !free_.empty()) {
            const Chunk chunk = free_.back();
            free_.pop_back();
            return chunk;
        }
        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = std::max(size, chunk_size),
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo allocation_info{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        Chunk chunk{.size = buffer_info.size};
        VmaAllocationInfo allocated{};
        check(vmaCreateBuffer(context_->allocator, &buffer_info, &allocation_info, &chunk.buffer,
                              &chunk.allocation, &allocated),
              "vmaCreateBuffer (Staging)");
        chunk.mapped = static_cast<std::byte*>(allocated.pMappedData);
        return chunk;
    }

    // Offener Command Buffer für Kopien; beginnt bei Bedarf einen neuen.
    VkCommandBuffer batch() {
        if (current_ != VK_NULL_HANDLE) {
            return current_;
        }
        if (free_commands_.empty()) {
            const VkCommandBufferAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = pool_,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            check(vkAllocateCommandBuffers(context_->device, &info, &cmd),
                  "vkAllocateCommandBuffers");
            free_commands_.push_back(cmd);
        }
        current_ = free_commands_.back();
        free_commands_.pop_back();
        check(vkResetCommandBuffer(current_, 0), "vkResetCommandBuffer");
        const VkCommandBufferBeginInfo begin_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        check(vkBeginCommandBuffer(current_, &begin_info), "vkBeginCommandBuffer");
        // Frühere Arbeit muss mit den Zielen fertig sein, bevor die Kopien sie überschreiben.
        full_barrier(current_);
        return current_;
    }

    DeviceContext* context_ = nullptr;
    bool transfer_only_ = false;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer current_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> free_commands_;
    std::vector<Batch> batches_;
    std::vector<Chunk> open_;
    std::vector<Chunk> in_flight_;
    std::vector<Chunk> free_;
};

} // namespace ngine::rhi::vulkan
