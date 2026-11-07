module;

#include "vk.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

// Gemeinsamer Zustand des Geräts, den alle Ressourcen kennen: Handles, die Zählung der Abgaben
// über einen Timeline Semaphore, die Löschliste und der bindlose Descriptor-Satz.
module ngine.rhi.vulkan:context;

import ngine.core;
import ngine.rhi;
import :common;

namespace ngine::rhi::vulkan {

// Gibt Vulkan-Objekte erst frei, wenn die GPU alle Abgaben bis zu ihrer Zerstörung abgearbeitet
// hat. Jeder Eintrag trägt den Wert der Timeline, ab dem er frei ist.
class DeletionQueue {
public:
    // transfer_value: Wert der Timeline der Transfer-Queue, den sie ebenfalls erreicht haben muss
    // (0 ohne Transfer-Queue, TD-17).
    void push(u64 retire_value, u64 transfer_value, std::function<void()> destroy) {
        const std::scoped_lock lock{mutex_};
        entries_.push_back({retire_value, transfer_value, std::move(destroy)});
    }

    // Gibt alles frei, was bis completed (und transfer_completed) fertig ist. Die Einträge sind
    // nach beiden Werten sortiert, weil beide nur wachsen.
    void collect(u64 completed, u64 transfer_completed = ~u64{0}) {
        std::vector<Entry> ready;
        {
            const std::scoped_lock lock{mutex_};
            const auto end = std::ranges::find_if(entries_, [&](const Entry& entry) {
                return entry.retire_value > completed || entry.transfer_value > transfer_completed;
            });
            ready.assign(std::make_move_iterator(entries_.begin()), std::make_move_iterator(end));
            entries_.erase(entries_.begin(), end);
        }
        for (Entry& entry : ready) {
            entry.destroy();
        }
    }

    // Nur nach vkDeviceWaitIdle.
    void flush() {
        collect(~u64{0});
    }

    [[nodiscard]] usize size() const {
        const std::scoped_lock lock{mutex_};
        return entries_.size();
    }

private:
    struct Entry {
        u64 retire_value;
        u64 transfer_value;
        std::function<void()> destroy;
    };
    mutable std::mutex mutex_;
    // Clang 20 instanziert std::deque in Modulen nicht zuverlässig; die Liste bleibt kurz.
    std::vector<Entry> entries_;
};

// Felder des bindlosen Descriptor-Satzes, in der Reihenfolge der Bindings (bindless.slang).
enum class BindlessKind : u8 { SampledImage = 0, Sampler = 1, StorageImage = 2 };
inline constexpr usize bindless_kind_count = 3;

// Ein Descriptor-Satz für alle Texturen und Sampler (ADR 0026). Freie Plätze verwaltet eine
// Freiliste pro Feld; zurück kommen sie erst über die Löschliste, wenn die GPU sie nicht mehr
// liest.
class BindlessHeap {
public:
    void init(VkPhysicalDevice physical_device, VkDevice device) {
        device_ = device;

        VkPhysicalDeviceVulkan12Properties v12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &v12};
        vkGetPhysicalDeviceProperties2(physical_device, &properties);
        capacity_[0] = std::min({u32{16384}, v12.maxDescriptorSetUpdateAfterBindSampledImages,
                                 v12.maxPerStageDescriptorUpdateAfterBindSampledImages});
        capacity_[1] = std::min({u32{256}, v12.maxDescriptorSetUpdateAfterBindSamplers,
                                 v12.maxPerStageDescriptorUpdateAfterBindSamplers});
        capacity_[2] = std::min({u32{4096}, v12.maxDescriptorSetUpdateAfterBindStorageImages,
                                 v12.maxPerStageDescriptorUpdateAfterBindStorageImages});

        const std::array<VkDescriptorType, bindless_kind_count> types{
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        std::array<VkDescriptorSetLayoutBinding, bindless_kind_count> bindings{};
        std::array<VkDescriptorBindingFlags, bindless_kind_count> binding_flags{};
        std::array<VkDescriptorPoolSize, bindless_kind_count> pool_sizes{};
        for (u32 i = 0; i < bindless_kind_count; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = types[i],
                           .descriptorCount = capacity_[i],
                           .stageFlags = VK_SHADER_STAGE_ALL};
            // Nicht jeder Platz ist belegt, und Plätze dürfen sich ändern, während andere
            // Frames im Flug den Satz benutzen.
            binding_flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                               VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                               VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
            pool_sizes[i] = {.type = types[i], .descriptorCount = capacity_[i]};
        }

        const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = static_cast<u32>(binding_flags.size()),
            .pBindingFlags = binding_flags.data(),
        };
        const VkDescriptorSetLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &flags_info,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        };
        check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &set_layout_),
              "vkCreateDescriptorSetLayout");

        const VkDescriptorPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
            .maxSets = 1,
            .poolSizeCount = static_cast<u32>(pool_sizes.size()),
            .pPoolSizes = pool_sizes.data(),
        };
        check(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_),
              "vkCreateDescriptorPool");

        const VkDescriptorSetAllocateInfo set_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool_,
            .descriptorSetCount = 1,
            .pSetLayouts = &set_layout_,
        };
        check(vkAllocateDescriptorSets(device_, &set_info, &set_), "vkAllocateDescriptorSets");

        // Ein Layout für alle Pipelines: der Satz und die Push Constants.
        const VkPushConstantRange push_constants{
            .stageFlags = VK_SHADER_STAGE_ALL,
            .offset = 0,
            .size = max_push_constant_size,
        };
        const VkPipelineLayoutCreateInfo pipeline_layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &set_layout_,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        };
        check(vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr, &pipeline_layout_),
              "vkCreatePipelineLayout");
    }

    void destroy() {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        vkDestroyDescriptorPool(device_, pool_, nullptr);
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    // Liefert invalid_index, wenn das Feld voll ist.
    [[nodiscard]] u32 allocate(BindlessKind kind) {
        const std::scoped_lock lock{mutex_};
        const auto k = static_cast<usize>(kind);
        if (!free_[k].empty()) {
            const u32 index = free_[k].back();
            free_[k].pop_back();
            return index;
        }
        return next_[k] < capacity_[k] ? next_[k]++ : invalid_index;
    }

    void release(BindlessKind kind, u32 index) {
        if (index == invalid_index) {
            return;
        }
        const std::scoped_lock lock{mutex_};
        free_[static_cast<usize>(kind)].push_back(index);
    }

    void write_image(BindlessKind kind, u32 index, VkImageView view) {
        const bool storage = kind == BindlessKind::StorageImage;
        const VkDescriptorImageInfo image{
            .imageView = view,
            .imageLayout =
                storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        write(kind, index, image);
    }

    void write_sampler(u32 index, VkSampler sampler) {
        write(BindlessKind::Sampler, index, VkDescriptorImageInfo{.sampler = sampler});
    }

    [[nodiscard]] VkDescriptorSet set() const {
        return set_;
    }
    [[nodiscard]] VkPipelineLayout pipeline_layout() const {
        return pipeline_layout_;
    }
    [[nodiscard]] u32 capacity(BindlessKind kind) const {
        return capacity_[static_cast<usize>(kind)];
    }

private:
    void write(BindlessKind kind, u32 index, const VkDescriptorImageInfo& image) {
        static constexpr std::array<VkDescriptorType, bindless_kind_count> types{
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set_,
            .dstBinding = static_cast<u32>(kind),
            .dstArrayElement = index,
            .descriptorCount = 1,
            .descriptorType = types[static_cast<usize>(kind)],
            .pImageInfo = &image,
        };
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;

    std::mutex mutex_;
    std::array<u32, bindless_kind_count> capacity_{};
    std::array<u32, bindless_kind_count> next_{};
    std::array<std::vector<u32>, bindless_kind_count> free_;
};

// Was Ressourcen vom Gerät brauchen. Lebt so lange wie das Device.
struct DeviceContext {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    // Zählt die Abgaben an die Queue: Abgabe n signalisiert den Wert n.
    VkSemaphore timeline = VK_NULL_HANDLE;
    u64 submitted = 0;
    DeletionQueue deletion;
    BindlessHeap bindless;
    // Grenze für anisotrope Filterung; 1, wenn die GPU sie nicht kann.
    f32 max_anisotropy = 1.0f;
    // Mit eigener Transfer-Queue (TD-17): beide Familien. Puffer im GPU-Speicher und Texturen mit
    // TransferDst gehören dann beiden Familien gleichzeitig (VK_SHARING_MODE_CONCURRENT), damit
    // keine Übergaben zwischen den Queues nötig sind.
    std::array<u32, 2> queue_families{};
    u32 queue_family_count = 1;
    // Wert, den die nächste Abgabe an die Transfer-Queue signalisiert; 0 ohne Transfer-Queue.
    u64 transfer_next = 0;

    // Wert der nächsten Abgabe. Eine jetzt zerstörte Ressource kann höchstens bis dahin benutzt
    // werden.
    [[nodiscard]] u64 next_submission() const {
        return submitted + 1;
    }

    // Gibt destroy frei, sobald die GPU mit allem fertig ist, was bis jetzt aufgezeichnet wurde.
    // extra_submissions verzögert weiter (z. B. für die Präsentation, die die Timeline nicht
    // abdeckt).
    void retire(std::function<void()> destroy, u64 extra_submissions = 0) {
        deletion.push(next_submission() + extra_submissions, transfer_next, std::move(destroy));
    }

    // Sharing-Modus für Ressourcen, die auch die Transfer-Queue beschreibt.
    void shared_with_transfer(VkSharingMode& mode, u32& count, const u32*& families) const {
        if (queue_family_count > 1) {
            mode = VK_SHARING_MODE_CONCURRENT;
            count = queue_family_count;
            families = queue_families.data();
        }
    }

    [[nodiscard]] u64 completed() const {
        u64 value = 0;
        check(vkGetSemaphoreCounterValue(device, timeline, &value), "vkGetSemaphoreCounterValue");
        return value;
    }

    void wait(u64 value) const {
        const VkSemaphoreWaitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &timeline,
            .pValues = &value,
        };
        check(vkWaitSemaphores(device, &wait_info, ~u64{0}), "vkWaitSemaphores");
    }
};

} // namespace ngine::rhi::vulkan
