module;

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Log-Konsole des Editors (EDIT-10): zeigt die Meldungen eines log::RingSink mit Filter nach Stufe
// und Text. Gebaut nur aus den Funktionen von :widgets; der Filter ist ohne UI prüfbar.
export module ngine.editor_ui:console;

import ngine.core;

export namespace ngine::editor_ui {

struct ConsoleFilter {
    bool trace = true;
    bool debug = true;
    bool info = true;
    bool warn = true;
    bool error = true;
    // Teiltext, ohne Rücksicht auf Groß- und Kleinschreibung (nur ASCII); leer: alles.
    std::string text;

    friend bool operator==(const ConsoleFilter&, const ConsoleFilter&) = default;
};

[[nodiscard]] bool matches(const log::RingSink::Entry& entry, const ConsoleFilter& filter);

class LogConsole {
public:
    explicit LogConsole(std::shared_ptr<log::RingSink> sink);

    // Zeichnet Filterleiste und Meldungen in das aktuelle Fenster (zwischen begin_window() und
    // end_window()).
    void draw();

    // Meldungen, die der Filter durchlässt, älteste zuerst (Stand des letzten refresh()).
    [[nodiscard]] const std::vector<log::RingSink::Entry>& visible() const noexcept {
        return visible_;
    }
    // Holt neue Meldungen aus dem Sink, falls sich Sink oder Filter geändert haben.
    void refresh();

    ConsoleFilter filter;
    // Springt ans Ende, solange man unten ist.
    bool auto_scroll = true;

private:
    std::shared_ptr<log::RingSink> sink_;
    std::vector<log::RingSink::Entry> visible_;
    std::uint64_t version_ = ~std::uint64_t{0};
    ConsoleFilter applied_;
    bool scroll_to_end_ = false;
};

} // namespace ngine::editor_ui
