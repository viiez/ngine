module;

#include "vk.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Das Vulkan-Device setzt die Teile des Backends zusammen: Instanz, GPU-Auswahl, Allokator,
// Timeline und Löschliste, bindloser Satz, Uploads, Frames, Swapchain und sofortige Ausführung.
module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;
import :common;
import :instance;
import :physical_device;
import :context;
import :resources;
import :command_list;
import :upload;
import :swapchain;

namespace ngine::rhi::vulkan {

namespace {

class VulkanDevice final : public Device {
public:
    VulkanDevice() = default;
    ~VulkanDevice() override;

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    std::expected<void, std::string> init(const DeviceDesc& desc);

    [[nodiscard]] const Capabilities& capabilities() const override {
        return physical_device_.capabilities;
    }

    [[nodiscard]] Result<std::unique_ptr<Texture>>
    create_texture(const TextureDesc& desc) override {
        return vulkan::create_texture(context_, desc);
    }
    [[nodiscard]] Result<std::unique_ptr<Shader>> create_shader(const ShaderDesc& desc) override {
        return vulkan::create_shader(context_.device, desc);
    }
    [[nodiscard]] Result<std::unique_ptr<Pipeline>>
    create_pipeline(const GraphicsPipelineDesc& desc) override {
        return vulkan::create_pipeline(context_, desc);
    }
    [[nodiscard]] Result<std::unique_ptr<Pipeline>>
    create_compute_pipeline(const ComputePipelineDesc& desc) override {
        return vulkan::create_compute_pipeline(context_, desc);
    }
    [[nodiscard]] Result<std::unique_ptr<Buffer>> create_buffer(const BufferDesc& desc) override {
        return vulkan::create_buffer(context_, desc);
    }
    [[nodiscard]] Result<std::unique_ptr<Sampler>>
    create_sampler(const SamplerDesc& desc) override {
        return vulkan::create_sampler(context_, desc);
    }

    void upload(Buffer& buffer, u64 offset, std::span<const std::byte> data) override {
        uploader_.upload(static_cast<const VulkanBuffer&>(buffer), offset, data);
    }
    void upload(Texture& texture, std::span<const std::byte> data) override {
        uploader_.upload(static_cast<const VulkanTexture&>(texture), data);
    }

    [[nodiscard]] UploadTicket upload_async(Buffer& buffer, u64 offset,
                                            std::span<const std::byte> data) override {
        const auto& target = static_cast<const VulkanBuffer&>(buffer);
        if (transfer_queue_ == VK_NULL_HANDLE) {
            uploader_.upload(target, offset, data);
            return {};
        }
        transfer_uploader_.upload(target, offset, data);
        return open_transfer_batch();
    }
    [[nodiscard]] UploadTicket upload_async(Texture& texture,
                                            std::span<const std::byte> data) override {
        const auto& target = static_cast<const VulkanTexture&>(texture);
        if (transfer_queue_ == VK_NULL_HANDLE) {
            uploader_.upload_levels(target, data);
            return {};
        }
        transfer_uploader_.upload_levels(target, data);
        return open_transfer_batch();
    }
    [[nodiscard]] bool upload_done(UploadTicket ticket) const override {
        return ticket.value <= transfer_seen_;
    }

    [[nodiscard]] Format swapchain_format() const override {
        return swapchain_ ? swapchain_->format() : Format::Unknown;
    }
    [[nodiscard]] u32 frames_in_flight() const override {
        return vulkan::frames_in_flight;
    }
    [[nodiscard]] std::optional<Frame> begin_frame() override;
    void end_frame() override;
    void resize(Extent2D extent) override {
        if (swapchain_) {
            swapchain_->resize(extent);
        }
    }

    void execute(const std::function<void(CommandList&)>& record) override;
    [[nodiscard]] std::vector<u8> read_texture(Texture& texture, ResourceState state) override;

    void wait_idle() override {
        vkDeviceWaitIdle(device_);
        collect();
    }

    [[nodiscard]] u64 gpu_timestamp() override;
    [[nodiscard]] std::optional<CalibratedTimestamp> calibrated_timestamp() override;

    [[nodiscard]] u32 validation_errors() const override {
        return validation_errors_.load(std::memory_order_relaxed);
    }

private:
    struct FrameData {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        // Wert der Timeline, den die letzte Abgabe dieses Frames signalisiert.
        u64 submitted = 0;
        // Zeitstempel, die die letzte Abgabe dieses Platzes geschrieben hat, und ihre Werte.
        u32 timestamp_count = 0;
        std::vector<u64> timestamps;
    };

    std::expected<void, std::string> create_allocator();
    void create_command_resources();
    VkCommandBuffer allocate_command_buffer(VkCommandPool pool);
    // Gibt frei, was die GPU nicht mehr braucht: Löschliste und Staging-Speicher.
    void collect();
    // Gibt cmd ab, davor die gesammelten Upload-Kopien. Signalisiert die Timeline mit dem Wert
    // der Abgabe, den die Funktion liefert, und optional signal. Vorher gehen die Kopien der
    // Transfer-Queue ab.
    u64 submit(VkCommandBuffer cmd, const VkSemaphoreSubmitInfo* wait, VkSemaphore signal);
    // Gibt die gesammelten Kopien an die Transfer-Queue ab (TD-17).
    void submit_transfer();
    // Ticket der Kopien, die mit der nächsten Abgabe an die Transfer-Queue gehen.
    UploadTicket open_transfer_batch() {
        context_.transfer_next = transfer_submitted_ + 1;
        return {transfer_submitted_ + 1};
    }

    // Muss vor instance_ stehen: Die Debug-Schicht zählt bis zum Zerstören der Instanz.
    std::atomic<u32> validation_errors_{0};
    // Wird als letztes Mitglied zerstört, nach allen Objekten des Geräts.
    Instance instance_;
    PhysicalDevice physical_device_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    // Gemeinsamer Zustand für alle Ressourcen; lebt so lange wie das Device.
    DeviceContext context_;
    Uploader uploader_;

    // Eigene Transfer-Queue (TD-17), nur wo die GPU eine Familie nur für Kopien hat. Ihre
    // Abgaben zählt eine eigene Timeline; transfer_seen_ ist der Stand, den die CPU zuletzt
    // gelesen hat. Jede Abgabe an die Grafik-Queue wartet auf ihn, deshalb sind alle Kopien bis
    // dahin für sie sichtbar.
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    Uploader transfer_uploader_;
    VkSemaphore transfer_timeline_ = VK_NULL_HANDLE;
    u64 transfer_submitted_ = 0;
    u64 transfer_seen_ = 0;

    // Nur mit Fenster vorhanden.
    std::unique_ptr<Swapchain> swapchain_;

    std::array<FrameData, vulkan::frames_in_flight> frames_{};
    u32 frame_index_ = 0;
    u32 image_index_ = 0;
    VulkanCommandList frame_commands_{context_.bindless};

    VkCommandPool immediate_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer immediate_cmd_ = VK_NULL_HANDLE;

    // Zeitstempel: max_frame_timestamps pro Platz, dahinter einer für gpu_timestamp(). Ohne
    // Zeitstempel auf der Queue VK_NULL_HANDLE.
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
};

std::expected<void, std::string> VulkanDevice::init(const DeviceDesc& desc) {
    if (!load_vulkan()) {
        return std::unexpected(
            "Vulkan-Loader nicht gefunden (ist ein Vulkan-Treiber installiert?)");
    }
    if (auto result = instance_.init(desc, validation_errors_); !result) {
        return result;
    }
    if (desc.surface) {
        if (auto result = instance_.create_surface(*desc.surface); !result) {
            return result;
        }
    }

    auto physical_device = pick_physical_device(
        instance_.handle(), instance_.surface(), desc.disabled_features,
        instance_.surface_maintenance1(), desc.transfer_queue, desc.bc_compression);
    if (!physical_device) {
        return std::unexpected(std::move(physical_device).error());
    }
    physical_device_ = *std::move(physical_device);

    const bool with_swapchain = instance_.surface() != VK_NULL_HANDLE;
    auto device = create_logical_device(physical_device_, with_swapchain);
    if (!device) {
        return std::unexpected(std::move(device).error());
    }
    device_ = *device;
    vkGetDeviceQueue(device_, physical_device_.queue_family, 0, &queue_);

    if (auto result = create_allocator(); !result) {
        return result;
    }
    context_.physical_device = physical_device_.handle;
    context_.max_anisotropy = physical_device_.capabilities.max_anisotropy;
    context_.device = device_;
    context_.allocator = allocator_;
    context_.bindless.init(physical_device_.handle, device_);
    create_command_resources();
    uploader_.init(context_, physical_device_.queue_family);
    if (const auto family = physical_device_.transfer_family) {
        vkGetDeviceQueue(device_, *family, 0, &transfer_queue_);
        context_.queue_families = {physical_device_.queue_family, *family};
        context_.queue_family_count = 2;
        transfer_uploader_.init(context_, *family, true);
        const VkSemaphoreTypeCreateInfo timeline_type{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };
        const VkSemaphoreCreateInfo timeline_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                                  .pNext = &timeline_type};
        check(vkCreateSemaphore(device_, &timeline_info, nullptr, &transfer_timeline_),
              "vkCreateSemaphore (Transfer)");
    }

    if (with_swapchain) {
        swapchain_ = std::make_unique<Swapchain>(
            context_, instance_.surface(), desc.surface->extent, desc.vsync,
            desc.present_fences && physical_device_.swapchain_maintenance1);
        if (auto result = swapchain_->rebuild(); !result) {
            return result;
        }
    }

    const Capabilities& caps = physical_device_.capabilities;
    log::info("Vulkan-Gerät: {} ({}), Vulkan {}", caps.device_name, to_string(caps.device_type),
              caps.api_version);
    log::info("Optionale Stufen: {}", describe(caps.features));
    log::info("Transfer-Queue für Uploads im Hintergrund: {}",
              transfer_queue_ != VK_NULL_HANDLE
                  ? std::format("ja (Familie {})", *physical_device_.transfer_family)
                  : std::string{"nein"});
    log::info("Blockkomprimierte Texturen (BC7): {}", caps.bc_compression ? "ja" : "nein");
    log::debug(
        "Present-Fences (VK_EXT_swapchain_maintenance1): {}, kalibrierte Zeitstempel: {}",
        with_swapchain && desc.present_fences && physical_device_.swapchain_maintenance1 ? "ja"
                                                                                         : "nein",
        physical_device_.calibrated_timestamps != nullptr ? physical_device_.calibrated_timestamps
                                                          : "nein");
    return {};
}

std::expected<void, std::string> VulkanDevice::create_allocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    const VmaAllocatorCreateInfo allocator_info{
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = physical_device_.handle,
        .device = device_,
        .pVulkanFunctions = &functions,
        .instance = instance_.handle(),
        .vulkanApiVersion = physical_device_.api_version,
    };
    if (const VkResult result = vmaCreateAllocator(&allocator_info, &allocator_);
        result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateAllocator", result));
    }
    return {};
}

VkCommandBuffer VulkanDevice::allocate_command_buffer(VkCommandPool pool) {
    const VkCommandBufferAllocateInfo info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device_, &info, &cmd), "vkAllocateCommandBuffers");
    return cmd;
}

void VulkanDevice::create_command_resources() {
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = physical_device_.queue_family,
    };
    const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    for (FrameData& frame : frames_) {
        check(vkCreateCommandPool(device_, &pool_info, nullptr, &frame.pool),
              "vkCreateCommandPool");
        frame.cmd = allocate_command_buffer(frame.pool);
        check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.image_available),
              "vkCreateSemaphore");
    }

    check(vkCreateCommandPool(device_, &pool_info, nullptr, &immediate_pool_),
          "vkCreateCommandPool");
    immediate_cmd_ = allocate_command_buffer(immediate_pool_);

    // Ein Timeline Semaphore zählt alle Abgaben; er ersetzt die Fences pro Frame.
    const VkSemaphoreTypeCreateInfo timeline_type{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo timeline_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                              .pNext = &timeline_type};
    check(vkCreateSemaphore(device_, &timeline_info, nullptr, &context_.timeline),
          "vkCreateSemaphore (Timeline)");

    if (physical_device_.timestamp_bits > 0) {
        const VkQueryPoolCreateInfo query_info{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = vulkan::frames_in_flight * max_frame_timestamps + 1,
        };
        check(vkCreateQueryPool(device_, &query_info, nullptr, &timestamps_), "vkCreateQueryPool");
    }
}

void VulkanDevice::collect() {
    const u64 completed = context_.completed();
    if (transfer_queue_ != VK_NULL_HANDLE) {
        check(vkGetSemaphoreCounterValue(device_, transfer_timeline_, &transfer_seen_),
              "vkGetSemaphoreCounterValue (Transfer)");
        transfer_uploader_.collect(transfer_seen_);
        context_.deletion.collect(completed, transfer_seen_);
    } else {
        context_.deletion.collect(completed);
    }
    uploader_.collect(completed);
}

void VulkanDevice::submit_transfer() {
    if (transfer_queue_ == VK_NULL_HANDLE || !transfer_uploader_.has_batch()) {
        return;
    }
    const u64 value = transfer_submitted_ + 1;
    const VkCommandBuffer copies = transfer_uploader_.finish(value);
    const VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                            .commandBuffer = copies};
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = transfer_timeline_,
                                       .value = value,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };
    check(vkQueueSubmit2(transfer_queue_, 1, &submit_info, VK_NULL_HANDLE),
          "vkQueueSubmit2 (Transfer)");
    transfer_submitted_ = value;
    context_.transfer_next = value;
}

u64 VulkanDevice::submit(VkCommandBuffer cmd, const VkSemaphoreSubmitInfo* wait,
                         VkSemaphore signal) {
    submit_transfer();
    const u64 value = context_.next_submission();
    std::array<VkCommandBufferSubmitInfo, 2> commands{};
    u32 command_count = 0;
    if (const VkCommandBuffer uploads = uploader_.finish(value); uploads != VK_NULL_HANDLE) {
        commands[command_count++] = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                     .commandBuffer = uploads};
    }
    commands[command_count++] = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                 .commandBuffer = cmd};

    std::array<VkSemaphoreSubmitInfo, 2> signals{};
    u32 signal_count = 0;
    signals[signal_count++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                               .semaphore = context_.timeline,
                               .value = value,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (signal != VK_NULL_HANDLE) {
        signals[signal_count++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .semaphore = signal,
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    }
    // Warten auf die Kopien der Transfer-Queue, die die CPU schon fertig gesehen hat: Das kostet
    // nichts und macht sie für diese Abgabe sichtbar (upload_done()).
    std::array<VkSemaphoreSubmitInfo, 2> waits{};
    u32 wait_count = 0;
    if (wait != nullptr) {
        waits[wait_count++] = *wait;
    }
    if (transfer_seen_ > 0) {
        waits[wait_count++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                               .semaphore = transfer_timeline_,
                               .value = transfer_seen_,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    }
    const VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait_count,
        .pWaitSemaphoreInfos = waits.data(),
        .commandBufferInfoCount = command_count,
        .pCommandBufferInfos = commands.data(),
        .signalSemaphoreInfoCount = signal_count,
        .pSignalSemaphoreInfos = signals.data(),
    };
    check(vkQueueSubmit2(queue_, 1, &submit_info, VK_NULL_HANDLE), "vkQueueSubmit2");
    context_.submitted = value;
    return value;
}

VulkanDevice::~VulkanDevice() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    swapchain_.reset();
    const usize leftover = context_.deletion.size();
    context_.deletion.flush();
    if (leftover > 0) {
        log::debug("Vulkan: {} Objekte beim Beenden aus der Löschliste freigegeben", leftover);
    }
    uploader_.destroy();
    transfer_uploader_.destroy();
    if (transfer_timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, transfer_timeline_, nullptr);
    }
    for (FrameData& frame : frames_) {
        vkDestroySemaphore(device_, frame.image_available, nullptr);
        vkDestroyCommandPool(device_, frame.pool, nullptr);
    }
    vkDestroyCommandPool(device_, immediate_pool_, nullptr);
    vkDestroyQueryPool(device_, timestamps_, nullptr);
    vkDestroySemaphore(device_, context_.timeline, nullptr);
    context_.bindless.destroy();
    if (allocator_ != nullptr) {
        vmaDestroyAllocator(allocator_);
    }
    vkDestroyDevice(device_, nullptr);
    // instance_ räumt sich danach als Mitglied selbst auf.
}

std::optional<Frame> VulkanDevice::begin_frame() {
    if (!swapchain_) {
        return std::nullopt;
    }
    if (swapchain_->needs_rebuild()) {
        if (auto result = swapchain_->rebuild(); !result) {
            log::error("{}", result.error());
            return std::nullopt;
        }
        if (swapchain_->needs_rebuild()) {
            return std::nullopt;
        }
    }

    // Der Frame mit demselben Platz muss fertig sein, bevor sein Command Buffer und sein
    // Semaphor wiederverwendet werden.
    FrameData& frame = frames_[frame_index_];
    context_.wait(frame.submitted);
    collect();

    const std::optional<u32> image_index = swapchain_->acquire(frame.image_available);
    if (!image_index) {
        return std::nullopt;
    }
    image_index_ = *image_index;

    check(vkResetCommandBuffer(frame.cmd, 0), "vkResetCommandBuffer");
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(frame.cmd, &begin_info), "vkBeginCommandBuffer");
    frame_commands_.reset(frame.cmd);

    // Zeitstempel des letzten Frames mit diesem Platz abholen (die GPU ist damit fertig), dann
    // die Abfragen für diesen Frame zurücksetzen.
    frame.timestamps.clear();
    if (timestamps_ != VK_NULL_HANDLE) {
        const u32 base = frame_index_ * max_frame_timestamps;
        if (frame.timestamp_count > 0) {
            frame.timestamps.resize(frame.timestamp_count);
            const VkResult result =
                vkGetQueryPoolResults(device_, timestamps_, base, frame.timestamp_count,
                                      frame.timestamps.size() * sizeof(u64),
                                      frame.timestamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
            if (result != VK_SUCCESS) {
                frame.timestamps.clear();
            }
            const u32 bits = physical_device_.timestamp_bits;
            if (bits < 64) {
                for (u64& value : frame.timestamps) {
                    value &= (u64{1} << bits) - 1;
                }
            }
        }
        vkCmdResetQueryPool(frame.cmd, timestamps_, base, max_frame_timestamps);
        frame_commands_.set_timestamps(timestamps_, base);
    }

    return Frame{.commands = frame_commands_,
                 .backbuffer = swapchain_->backbuffer(image_index_),
                 .index = frame_index_,
                 .timestamps = frame.timestamps};
}

void VulkanDevice::end_frame() {
    FrameData& frame = frames_[frame_index_];
    frame.timestamp_count = frame_commands_.timestamp_count();
    frame_commands_.set_timestamps(VK_NULL_HANDLE, 0);
    check(vkEndCommandBuffer(frame.cmd), "vkEndCommandBuffer");

    // Das Bild ist erst nach dem Acquire frei; vorher darf nichts hineinschreiben. Die erste
    // Barriere auf den Backbuffer wartet auf alle Stufen und hängt damit an dieser Wartestufe.
    const VkSemaphoreSubmitInfo wait{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = frame.image_available,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    frame.submitted = submit(frame.cmd, &wait, swapchain_->render_finished(image_index_));
    swapchain_->present(queue_, image_index_);

    frame_index_ = (frame_index_ + 1) % vulkan::frames_in_flight;
}

void VulkanDevice::execute(const std::function<void(CommandList&)>& record) {
    check(vkResetCommandBuffer(immediate_cmd_, 0), "vkResetCommandBuffer");
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(immediate_cmd_, &begin_info), "vkBeginCommandBuffer");

    VulkanCommandList commands{context_.bindless};
    commands.reset(immediate_cmd_);
    record(commands);

    check(vkEndCommandBuffer(immediate_cmd_), "vkEndCommandBuffer");
    // Wartet absichtlich: execute() ist für Werkzeuge und Tests gedacht, nicht für Frames. Auch
    // auf die Transfer-Queue, damit Uploads im Hintergrund danach erledigt sind.
    context_.wait(submit(immediate_cmd_, nullptr, VK_NULL_HANDLE));
    if (transfer_submitted_ > 0) {
        const VkSemaphoreWaitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &transfer_timeline_,
            .pValues = &transfer_submitted_,
        };
        check(vkWaitSemaphores(device_, &wait_info, ~u64{0}), "vkWaitSemaphores (Transfer)");
    }
    collect();
}

u64 VulkanDevice::gpu_timestamp() {
    if (timestamps_ == VK_NULL_HANDLE) {
        return 0;
    }
    const u32 query = vulkan::frames_in_flight * max_frame_timestamps;
    execute([&](CommandList& commands) {
        const VkCommandBuffer cmd = static_cast<VulkanCommandList&>(commands).handle();
        vkCmdResetQueryPool(cmd, timestamps_, query, 1);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps_, query);
    });
    u64 value = 0;
    check(vkGetQueryPoolResults(device_, timestamps_, query, 1, sizeof(value), &value,
                                sizeof(value), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
          "vkGetQueryPoolResults");
    const u32 bits = physical_device_.timestamp_bits;
    return bits < 64 ? value & ((u64{1} << bits) - 1) : value;
}

std::optional<CalibratedTimestamp> VulkanDevice::calibrated_timestamp() {
    const char* extension = physical_device_.calibrated_timestamps;
    if (extension == nullptr) {
        return std::nullopt;
    }
    const PFN_vkGetCalibratedTimestampsKHR get =
        std::string_view{extension} == VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
            ? vkGetCalibratedTimestampsKHR
            : vkGetCalibratedTimestampsEXT;
    if (get == nullptr) {
        return std::nullopt;
    }
    // Nur die Uhr der GPU; die der CPU liest steady_clock davor und danach, die Mitte gilt.
    const VkCalibratedTimestampInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
        .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR,
    };
    u64 gpu = 0;
    u64 deviation = 0;
    const auto before = std::chrono::steady_clock::now();
    if (get(device_, 1, &info, &gpu, &deviation) != VK_SUCCESS) {
        return std::nullopt;
    }
    const auto after = std::chrono::steady_clock::now();
    // Wurde der Thread dazwischen unterbrochen, taugt die Messung nicht.
    if (after - before > std::chrono::microseconds{200}) {
        return std::nullopt;
    }
    const u32 bits = physical_device_.timestamp_bits;
    const auto middle = before + (after - before) / 2;
    return CalibratedTimestamp{
        .gpu = bits < 64 ? gpu & ((u64{1} << bits) - 1) : gpu,
        .cpu_ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(middle.time_since_epoch())
                .count())};
}

std::vector<u8> VulkanDevice::read_texture(Texture& texture, ResourceState state) {
    const auto& source = static_cast<const VulkanTexture&>(texture);
    verify(!is_depth(source.format()), "Tiefenpuffer lassen sich nicht auslesen");
    verify(has(source.usage(), TextureUsage::TransferSrc), "read_texture() braucht TransferSrc");
    verify(state != ResourceState::Undefined, "read_texture(): Inhalt ist undefiniert");
    const Extent2D extent = source.extent();
    const VkDeviceSize size =
        VkDeviceSize{extent.width} * extent.height * bytes_per_pixel(source.format());

    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation_info{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo allocated{};
    check(vmaCreateBuffer(allocator_, &buffer_info, &allocation_info, &buffer, &allocation,
                          &allocated),
          "vmaCreateBuffer");

    execute([&](CommandList& commands) {
        const VkCommandBuffer cmd = static_cast<VulkanCommandList&>(commands).handle();
        // Der Zielpuffer kann Speicher einer früheren Rücklese wiederverwenden; deren Kopie muss
        // vor dieser fertig sein.
        full_barrier(cmd);
        commands.barrier(source, state, ResourceState::TransferSrc);
        const VkBufferImageCopy region{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {extent.width, extent.height, 1},
        };
        vkCmdCopyImageToBuffer(cmd, source.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                               &region);
        // Kopierte Daten für die CPU sichtbar machen.
        const VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &dependency);
        if (state != ResourceState::TransferSrc) {
            commands.barrier(source, ResourceState::TransferSrc, state);
        }
    });

    check(vmaInvalidateAllocation(allocator_, allocation, 0, VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    std::vector<u8> pixels(static_cast<usize>(size));
    std::memcpy(pixels.data(), allocated.pMappedData, pixels.size());
    vmaDestroyBuffer(allocator_, buffer, allocation);
    return pixels;
}

} // namespace

Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc) {
    auto device = std::make_unique<VulkanDevice>();
    if (auto result = device->init(desc); !result) {
        return std::unexpected(result.error());
    }
    return device;
}

} // namespace ngine::rhi::vulkan
