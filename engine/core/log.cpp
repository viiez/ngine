module;

#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

module ngine.core;

namespace ngine::log {

namespace {

#ifdef NDEBUG
constexpr Level default_level = Level::Info;
#else
constexpr Level default_level = Level::Debug;
#endif

struct State {
    std::mutex mutex;
    std::vector<std::shared_ptr<Sink>> sinks{std::make_shared<ConsoleSink>()};
    std::atomic<Level> min_level{default_level};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

State& state() {
    static State instance;
    return instance;
}

} // namespace

std::string_view to_string(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Fatal: return "fatal";
    case Level::Off: return "off";
    }
    return "unknown";
}

void ConsoleSink::write(const Record& record) {
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(record.uptime).count();
    const auto line = std::format("[{:>6}.{:03}] [{:<5}] {}\n", millis / 1000, millis % 1000,
                                  to_string(record.level), record.message);
    std::FILE* stream = record.level >= Level::Warn ? stderr : stdout;
    if (stream == stderr) {
        // stdout ist gepuffert, stderr nicht. Ohne flush landen Warnungen vor älteren Zeilen.
        std::fflush(stdout);
    }
    std::fwrite(line.data(), 1, line.size(), stream);
}

void ConsoleSink::flush() {
    std::fflush(stdout);
    std::fflush(stderr);
}

void add_sink(std::shared_ptr<Sink> sink) {
    if (!sink) {
        return;
    }
    auto& s = state();
    const std::scoped_lock lock{s.mutex};
    s.sinks.push_back(std::move(sink));
}

void clear_sinks() {
    auto& s = state();
    const std::scoped_lock lock{s.mutex};
    s.sinks.clear();
}

void set_level(Level level) noexcept {
    state().min_level.store(level, std::memory_order_relaxed);
}

Level level() noexcept {
    return state().min_level.load(std::memory_order_relaxed);
}

bool enabled(Level level) noexcept {
    return level != Level::Off && level >= log::level();
}

void write(Level level, std::string_view message) {
    if (!enabled(level)) {
        return;
    }
    auto& s = state();
    const Record record{level, message, std::chrono::steady_clock::now() - s.start};
    const std::scoped_lock lock{s.mutex};
    for (const auto& sink : s.sinks) {
        sink->write(record);
        if (level == Level::Fatal) {
            sink->flush();
        }
    }
}

void flush() {
    auto& s = state();
    const std::scoped_lock lock{s.mutex};
    for (const auto& sink : s.sinks) {
        sink->flush();
    }
}

} // namespace ngine::log
