module;

#include "vk.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

// Hochladen in GPU-Speicher über Staging-Puffer (TD-11). Kopien sammeln sich in einem eigenen
// Command Buffer, den das Device vor der nächsten Abgabe auf derselben Queue einreiht. Niemand
// wartet auf die GPU: Staging-Speicher und Command Buffer kommen über die Timeline zurück.
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

    void init(DeviceContext& context, u32 queue_family) {
        context_ = &context;
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
        transition(ResourceState::TransferDst, ResourceState::ShaderRead);
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
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer current_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> free_commands_;
    std::vector<Batch> batches_;
    std::vector<Chunk> open_;
    std::vector<Chunk> in_flight_;
    std::vector<Chunk> free_;
};

} // namespace ngine::rhi::vulkan
