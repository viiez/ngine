#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
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
