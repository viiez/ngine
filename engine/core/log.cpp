module;

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

module ngine.core;

import :console;

namespace ngine::log {

namespace {

#ifdef NDEBUG
constexpr Level default_level = Level::Info;
#else
constexpr Level default_level = Level::Debug;
#endif

struct State {
    // Läuft vor dem ersten Log, weil state() beim ersten Log angelegt wird.
    bool console_ready = (detail::prepare_console(), true);
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

std::string format_line(const Record& record) {
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(record.uptime).count();
    return std::format("[{:>6}.{:03}] [{:<5}] {}", millis / 1000, millis % 1000,
                       to_string(record.level), record.message);
}

void ConsoleSink::write(const Record& record) {
    std::string line = format_line(record);
    line += '\n';
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

std::expected<std::shared_ptr<FileSink>, std::string> FileSink::create(std::string_view utf8_path) {
    // Der Konstruktor mit char8_t liest den Pfad auf jeder Plattform als UTF-8.
    const std::filesystem::path path{std::u8string{utf8_path.begin(), utf8_path.end()}};
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::FILE* file = detail::open_log_file(utf8_path);
    if (file == nullptr) {
        return std::unexpected(std::format("Log-Datei lässt sich nicht anlegen: {}", utf8_path));
    }
    return std::shared_ptr<FileSink>{new FileSink{file}};
}

FileSink::~FileSink() {
    std::fclose(file_);
}

void FileSink::write(const Record& record) {
    std::string line = format_line(record);
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), file_);
    if (record.level >= Level::Error) {
        std::fflush(file_);
    }
}

void FileSink::flush() {
    std::fflush(file_);
}

struct RingSink::State {
    std::mutex mutex;
    std::size_t capacity = 1;
    std::deque<Entry> entries;
    std::uint64_t version = 0;
    std::uint64_t dropped = 0;
};

RingSink::RingSink(std::size_t capacity) : state_{std::make_unique<State>()} {
    state_->capacity = capacity > 0 ? capacity : 1;
}

RingSink::~RingSink() = default;

void RingSink::write(const Record& record) {
    const std::scoped_lock lock{state_->mutex};
    if (state_->entries.size() >= state_->capacity) {
        state_->entries.pop_front();
        ++state_->dropped;
    }
    state_->entries.push_back({record.level, record.uptime, std::string{record.message}});
    ++state_->version;
}

std::vector<RingSink::Entry> RingSink::snapshot() const {
    const std::scoped_lock lock{state_->mutex};
    return {state_->entries.begin(), state_->entries.end()};
}

std::uint64_t RingSink::version() const {
    const std::scoped_lock lock{state_->mutex};
    return state_->version;
}

std::uint64_t RingSink::dropped() const {
    const std::scoped_lock lock{state_->mutex};
    return state_->dropped;
}

void RingSink::clear() {
    const std::scoped_lock lock{state_->mutex};
    state_->entries.clear();
    ++state_->version;
}

void add_sink(std::shared_ptr<Sink> sink) {
    if (!sink) {
        return;
    }
    auto& s = state();
    const std::scoped_lock lock{s.mutex};
    s.sinks.push_back(std::move(sink));
}

void remove_sink(const std::shared_ptr<Sink>& sink) {
    auto& s = state();
    const std::scoped_lock lock{s.mutex};
    std::erase(s.sinks, sink);
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
