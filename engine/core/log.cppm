module;

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Zeile, wie ConsoleSink und FileSink sie schreiben: Zeit seit dem Start, Stufe, Text, ohne
// Zeilenumbruch.
[[nodiscard]] std::string format_line(const Record& record);

// Schreibt alle Meldungen in eine Datei (CORE-12), UTF-8, eine Zeile pro Meldung. Ab Error wird
// sofort auf die Platte geschrieben, damit die Meldung einen Absturz übersteht.
class FileSink final : public Sink {
public:
    // Legt die Datei neu an (ein vorhandener Inhalt geht verloren), fehlende Ordner auch. Der Pfad
    // ist UTF-8.
    [[nodiscard]] static std::expected<std::shared_ptr<FileSink>, std::string>
    create(std::string_view path);
    ~FileSink() override;

    void write(const Record& record) override;
    void flush() override;

private:
    explicit FileSink(std::FILE* file) : file_{file} {}
    std::FILE* file_;
};

// Hält die letzten Meldungen im Speicher, z. B. für die Log-Konsole des Editors (EDIT-10). Darf
// aus jedem Thread beschrieben und gelesen werden.
class RingSink final : public Sink {
public:
    struct Entry {
        Level level = Level::Info;
        std::chrono::steady_clock::duration uptime{};
        std::string message;
    };

    explicit RingSink(std::size_t capacity = 2000);
    ~RingSink() override;

    void write(const Record& record) override;

    // Kopie der gehaltenen Meldungen, älteste zuerst. Eine Kopie, damit wer sie anzeigt, dabei
    // selbst loggen darf.
    [[nodiscard]] std::vector<Entry> snapshot() const;
    // Zählt jede neue Meldung und jedes clear(); ändert sich der Wert nicht, ist auch der Inhalt
    // gleich geblieben.
    [[nodiscard]] std::uint64_t version() const;
    // Meldungen, die wegen der Kapazität verworfen wurden.
    [[nodiscard]] std::uint64_t dropped() const;
    void clear();

private:
    // Zustand mit Mutex in der Implementierung: <mutex> im globalen Modulfragment der
    // Schnittstelle bringt MSVC beim Import von ngine.core aus dem Tritt (C1116 mit <stop_token>).
    struct State;
    std::unique_ptr<State> state_;
};

void add_sink(std::shared_ptr<Sink> sink);
// Trägt einen Sink wieder aus; danach bekommt er keine Meldungen mehr.
void remove_sink(const std::shared_ptr<Sink>& sink);
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
