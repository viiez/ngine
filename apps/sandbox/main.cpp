// Sandbox: kleine Testanwendung, an der jede Stufe sichtbar wird.

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

import ngine.core;

int main(int argc, char** argv) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const bool headless = std::ranges::any_of(
        args, [](const char* arg) { return std::string_view{arg} == "--headless"; });

    const auto version = ngine::engine_version;
    ngine::log::info("{} {}.{}.{} startet", ngine::engine_name, version.major, version.minor,
                     version.patch);
    ngine::log::info("Modus: {}", headless ? "ohne Fenster (Server)" : "mit Fenster");

    if (!headless) {
        ngine::log::warn("Fenster und Vulkan kommen im folgenden Schritt von Stufe 0");
    }

    ngine::log::info("Beendet");
    ngine::log::flush();
    return 0;
}
