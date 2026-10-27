module;

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string_view>
#include <utility>

export module ngine.core:log;

// Logging der Engine. Ausgaben gehen an austauschbare Sinks; standardmäßig ist
// ein ConsoleSink eingetragen. Eine Fremdbibliothek wie spdlog könnte später
// als weiterer Sink angebunden werden, ohne dass sich Aufrufer ändern.
export namespace ngine::log {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error, Fatal, Off };

[[nodiscard]] std::string_view to_string(Level level) noexcept;

struct Record {
    Level level;
    std::string_view message;
    // Zeit seit dem Start des Loggings.
    std::chrono::steady_clock::duration uptime;
};

class Sink {
public:
    Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    virtual ~Sink() = default;

    virtual void write(const Record& record) = 0;
    virtual void flush() {}
};

// Schreibt Warn und höher nach stderr, alles andere nach stdout.
class ConsoleSink final : public Sink {
public:
    void write(const Record& record) override;
    void flush() override;
};

void add_sink(std::shared_ptr<Sink> sink);
void clear_sinks();

void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
[[nodiscard]] bool enabled(Level level) noexcept;

void write(Level level, std::string_view message);
void flush();

template <typename... Args>
void message(Level level, std::format_string<Args...> fmt, Args&&... args) {
    if (!enabled(level)) {
        return;
    }
    write(level, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::Trace, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::Debug, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::Info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::Warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::Error, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void fatal(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::Fatal, fmt, std::forward<Args>(args)...);
}

} // namespace ngine::log
