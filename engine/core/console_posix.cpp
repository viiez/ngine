module;

#include <cstdio>
#include <string>
#include <string_view>

module ngine.core;

import :console;

namespace ngine::log::detail {

// Terminals unter Linux erwarten UTF-8; nichts zu tun.
void prepare_console() noexcept {}

std::FILE* open_log_file(std::string_view utf8_path) noexcept {
    const std::string path{utf8_path};
    return std::fopen(path.c_str(), "wb");
}

} // namespace ngine::log::detail
