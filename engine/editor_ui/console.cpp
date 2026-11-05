module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.editor_ui;

import ngine.core;

namespace ngine::editor_ui {

namespace {

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool contains_ignore_case(std::string_view text, std::string_view part) {
    if (part.empty()) {
        return true;
    }
    const auto found =
        std::ranges::search(text, part, [](char a, char b) { return lower(a) == lower(b); });
    return !found.empty();
}

UiColor level_color(log::Level level) {
    switch (level) {
    case log::Level::Trace:
    case log::Level::Debug: return {0.6f, 0.6f, 0.6f, 1.0f};
    case log::Level::Warn: return {1.0f, 0.8f, 0.3f, 1.0f};
    case log::Level::Error:
    case log::Level::Fatal: return {1.0f, 0.4f, 0.4f, 1.0f};
    default: return {0.9f, 0.9f, 0.9f, 1.0f};
    }
}

} // namespace

bool matches(const log::RingSink::Entry& entry, const ConsoleFilter& filter) {
    bool level = true;
    switch (entry.level) {
    case log::Level::Trace: level = filter.trace; break;
    case log::Level::Debug: level = filter.debug; break;
    case log::Level::Info: level = filter.info; break;
    case log::Level::Warn: level = filter.warn; break;
    // Fatale Meldungen zählen als Fehler.
    case log::Level::Error:
    case log::Level::Fatal: level = filter.error; break;
    case log::Level::Off: level = false; break;
    }
    return level && contains_ignore_case(entry.message, filter.text);
}

LogConsole::LogConsole(std::shared_ptr<log::RingSink> sink) : sink_{std::move(sink)} {}

void LogConsole::refresh() {
    const std::uint64_t version = sink_->version();
    if (version == version_ && filter == applied_) {
        return;
    }
    version_ = version;
    applied_ = filter;
    visible_.clear();
    for (log::RingSink::Entry& entry : sink_->snapshot()) {
        if (matches(entry, filter)) {
            visible_.push_back(std::move(entry));
        }
    }
    scroll_to_end_ = auto_scroll;
}

void LogConsole::draw() {
    if (button("Leeren")) {
        sink_->clear();
    }
    same_line();
    checkbox("Trace", filter.trace);
    same_line();
    checkbox("Debug", filter.debug);
    same_line();
    checkbox("Info", filter.info);
    same_line();
    checkbox("Warn", filter.warn);
    same_line();
    checkbox("Fehler", filter.error);
    same_line();
    checkbox("Mitlaufen", auto_scroll);
    same_line();
    set_next_item_width(-1.0f);
    input_text("##filter", filter.text);
    separator();

    refresh();
    if (begin_child("##log")) {
        // Wer hochgescrollt hat, liest gerade; dann nicht ans Ende springen.
        const bool at_end = scroll_y() >= scroll_max_y() - 1.0f;
        clipped_rows(visible_.size(), [&](usize first, usize last) {
            for (usize i = first; i < last; ++i) {
                const log::RingSink::Entry& entry = visible_[i];
                const auto millis =
                    std::chrono::duration_cast<std::chrono::milliseconds>(entry.uptime).count();
                text_colored(
                    level_color(entry.level),
                    std::format("[{:>6}.{:03}] {}", millis / 1000, millis % 1000, entry.message));
            }
        });
        if (scroll_to_end_ && at_end) {
            scroll_here(1.0f);
        }
        scroll_to_end_ = false;
    }
    end_child();
}

} // namespace ngine::editor_ui
