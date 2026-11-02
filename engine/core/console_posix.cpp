module ngine.core;

import :console;

namespace ngine::log::detail {

// Terminals unter Linux erwarten UTF-8; nichts zu tun.
void prepare_console() noexcept {}

} // namespace ngine::log::detail
