module;

#include <tracy/TracyC.h>

#include <cstddef>
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

} // namespace ngine::profile
