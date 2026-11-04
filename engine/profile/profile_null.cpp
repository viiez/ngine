module;

#include <string_view>

// Ohne Tracy: alles leer.
module ngine.profile;

namespace ngine::profile {

Zone::Zone(const SourceLocation&) noexcept : context_{} {}
Zone::~Zone() = default;

void set_thread_name(const char*) noexcept {}
void frame_mark() noexcept {}
void plot(const char*, f64) noexcept {}
void message(std::string_view) noexcept {}

u8 gpu_context(std::string_view, i64, f32) noexcept {
    return 0;
}
void gpu_zone_begin(u8, u16, std::string_view) noexcept {}
void gpu_zone_end(u8, u16) noexcept {}
void gpu_time(u8, u16, i64) noexcept {}

} // namespace ngine::profile
