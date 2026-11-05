#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

import ngine.core;

namespace {

struct CapturedRecord {
    ngine::log::Level level;
    std::string message;
};

class CaptureSink final : public ngine::log::Sink {
public:
    void write(const ngine::log::Record& record) override {
        records.push_back({record.level, std::string{record.message}});
    }

    std::vector<CapturedRecord> records;
};

// Ersetzt für die Dauer eines Tests alle Sinks durch einen CaptureSink.
struct CaptureScope {
    CaptureScope() {
        ngine::log::clear_sinks();
        ngine::log::add_sink(sink);
        ngine::log::set_level(ngine::log::Level::Trace);
    }

    ~CaptureScope() {
        ngine::log::clear_sinks();
        ngine::log::add_sink(std::make_shared<ngine::log::ConsoleSink>());
        ngine::log::set_level(ngine::log::Level::Info);
    }

    CaptureScope(const CaptureScope&) = delete;
    CaptureScope& operator=(const CaptureScope&) = delete;

    std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
};

} // namespace

TEST_CASE("Log formatiert Nachrichten und leitet sie an Sinks", "[core][log]") {
    const CaptureScope scope;

    ngine::log::info("Chunk {} geladen in {} ms", 42, 3);

    REQUIRE(scope.sink->records.size() == 1);
    CHECK(scope.sink->records[0].level == ngine::log::Level::Info);
    CHECK(scope.sink->records[0].message == "Chunk 42 geladen in 3 ms");
}

TEST_CASE("Log verwirft Nachrichten unter der Mindeststufe", "[core][log]") {
    const CaptureScope scope;
    ngine::log::set_level(ngine::log::Level::Warn);

    ngine::log::debug("unsichtbar");
    ngine::log::info("unsichtbar");
    ngine::log::warn("sichtbar");
    ngine::log::error("auch sichtbar");

    REQUIRE(scope.sink->records.size() == 2);
    CHECK(scope.sink->records[0].level == ngine::log::Level::Warn);
    CHECK(scope.sink->records[1].level == ngine::log::Level::Error);
}

TEST_CASE("Stufe Off schaltet das Logging ab", "[core][log]") {
    const CaptureScope scope;
    ngine::log::set_level(ngine::log::Level::Off);

    ngine::log::fatal("unsichtbar");

    CHECK(scope.sink->records.empty());
    CHECK_FALSE(ngine::log::enabled(ngine::log::Level::Fatal));
}

TEST_CASE("Stufen haben lesbare Namen", "[core][log]") {
    CHECK(ngine::log::to_string(ngine::log::Level::Trace) == "trace");
    CHECK(ngine::log::to_string(ngine::log::Level::Warn) == "warn");
    CHECK(ngine::log::to_string(ngine::log::Level::Fatal) == "fatal");
}

namespace {

ngine::log::Record record(ngine::log::Level level, std::string_view message) {
    return {level, message, std::chrono::milliseconds{1500}};
}

} // namespace

TEST_CASE("RingSink keeps the newest messages up to its capacity", "[core][log]") {
    ngine::log::RingSink ring{3};
    const auto start = ring.version();
    for (const char* message : {"eins", "zwei", "drei", "vier"}) {
        ring.write(record(ngine::log::Level::Info, message));
    }

    const auto entries = ring.snapshot();
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].message == "zwei");
    CHECK(entries[2].message == "vier");
    CHECK(entries[2].uptime == std::chrono::milliseconds{1500});
    CHECK(ring.dropped() == 1);
    CHECK(ring.version() == start + 4);

    ring.clear();
    CHECK(ring.snapshot().empty());
    CHECK(ring.version() == start + 5);
}

TEST_CASE("RingSink receives messages through the log", "[core][log]") {
    const CaptureScope scope;
    const auto ring = std::make_shared<ngine::log::RingSink>();
    ngine::log::add_sink(ring);
    ngine::log::warn("Achtung {}", 7);
    ngine::log::remove_sink(ring);
    ngine::log::warn("nicht mehr");

    const auto entries = ring->snapshot();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].level == ngine::log::Level::Warn);
    CHECK(entries[0].message == "Achtung 7");
    CHECK(scope.sink->records.size() == 2);
}

TEST_CASE("FileSink writes formatted lines into a file", "[core][log]") {
    const auto dir = std::filesystem::temp_directory_path() / "ngine-log-tests";
    std::filesystem::remove_all(dir);
    const auto path = dir / "unter" / "test.log";
    const auto utf8 = path.u8string();
    {
        auto sink = ngine::log::FileSink::create(
            std::string_view{reinterpret_cast<const char*>(utf8.data()), utf8.size()});
        REQUIRE(sink);
        (*sink)->write(record(ngine::log::Level::Info, "Hallo Datei"));
        (*sink)->write(record(ngine::log::Level::Error, "Fehler mit Ümlaut"));
    }

    std::ifstream file{path, std::ios::binary};
    REQUIRE(file);
    std::string first;
    std::string second;
    std::getline(file, first);
    std::getline(file, second);
    CHECK(first == "[     1.500] [info ] Hallo Datei");
    CHECK(second == "[     1.500] [error] Fehler mit Ümlaut");
    file.close();
    std::filesystem::remove_all(dir);
}
