module;

#include <tracy/TracyC.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Adapter für Tracy über dessen C-Schnittstelle. Nur diese Datei kennt Tracy.
module ngine.profile;

namespace ngine::profile {

namespace {

// SourceLocation hat dasselbe Layout wie Tracys ___tracy_source_location_data, damit die
// statischen Daten der Makros ohne Kopie an Tracy gehen.
using TracyLocation = ___tracy_source_location_data;
static_assert(sizeof(SourceLocation) == sizeof(TracyLocation));
static_assert(offsetof(SourceLocation, name) == offsetof(TracyLocation, name));
static_assert(offsetof(SourceLocation, function) == offsetof(TracyLocation, function));
static_assert(offsetof(SourceLocation, file) == offsetof(TracyLocation, file));
static_assert(offsetof(SourceLocation, line) == offsetof(TracyLocation, line));
static_assert(offsetof(SourceLocation, color) == offsetof(TracyLocation, color));

static_assert(sizeof(TracyCZoneCtx) <= 16 && alignof(TracyCZoneCtx) <= 8,
              "Zone::context_ ist zu klein für Tracys Zonen-Kontext");

} // namespace

Zone::Zone(const SourceLocation& location) noexcept {
    const TracyCZoneCtx context =
        ___tracy_emit_zone_begin(reinterpret_cast<const TracyLocation*>(&location), 1);
    std::memcpy(context_, &context, sizeof(context));
}

Zone::~Zone() {
    TracyCZoneCtx context;
    std::memcpy(&context, context_, sizeof(context));
    ___tracy_emit_zone_end(context);
}

void set_thread_name(const char* name) noexcept {
    ___tracy_set_thread_name(name);
}

void frame_mark() noexcept {
    ___tracy_emit_frame_mark(nullptr);
}

void plot(const char* name, f64 value) noexcept {
    ___tracy_emit_plot(name, value);
}

void message(std::string_view text) noexcept {
    TracyCMessage(text.data(), text.size());
}

u8 gpu_context(std::string_view name, i64 gpu_time, f32 period) noexcept {
    // Tracy unterscheidet Kontexte nur über diese Nummer.
    static std::atomic<u8> next{0};
    const u8 context = next.fetch_add(1, std::memory_order_relaxed);
    // Typ 2: Vulkan; Tracy zeigt ihn nur an.
    ___tracy_emit_gpu_new_context_serial(
        {.gpuTime = gpu_time, .period = period, .context = context, .flags = 0, .type = 2});
    ___tracy_emit_gpu_context_name_serial(
        {.context = context, .name = name.data(), .len = static_cast<uint16_t>(name.size())});
    return context;
}

void gpu_zone_begin(u8 context, u16 query, std::string_view name) noexcept {
    static constexpr std::string_view file = "GPU";
    const uint64_t location = ___tracy_alloc_srcloc_name(0, file.data(), file.size(), name.data(),
                                                         name.size(), name.data(), name.size(), 0);
    ___tracy_emit_gpu_zone_begin_alloc_serial(
        {.srcloc = location, .queryId = query, .context = context});
}

void gpu_zone_end(u8 context, u16 query) noexcept {
    ___tracy_emit_gpu_zone_end_serial({.queryId = query, .context = context});
}

void gpu_time(u8 context, u16 query, i64 gpu_time) noexcept {
    ___tracy_emit_gpu_time_serial({.gpuTime = gpu_time, .queryId = query, .context = context});
}

} // namespace ngine::profile
