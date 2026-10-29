module;

#include <cstdlib>
#include <source_location>
#include <string_view>

module ngine.core;

namespace ngine {

void verify(bool condition, std::string_view message, std::source_location location) {
    if (condition) {
        return;
    }
    log::fatal("Prüfung fehlgeschlagen: {} ({}:{})", message, location.file_name(),
               location.line());
    log::flush();
    std::abort();
}

} // namespace ngine
