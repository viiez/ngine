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

} // namespace ngine::profile
