module;

#include <map>
#include <string>
#include <string_view>
#include <vector>

export module ngine.filesystem:watcher;

import ngine.core;

export namespace ngine::filesystem {

enum class FileChange : u8 { Added, Modified, Removed };

struct FileEvent {
    // Normalisierter, absoluter Pfad (normalize()).
    std::string path;
    FileChange change = FileChange::Modified;
};

// Beobachtet einen Ordner samt Unterordnern auf geänderte Dateien (PLAT-06, ADR 0034). Er fragt
// Größe und Änderungszeit ab (Polling) statt Benachrichtigungen des Betriebssystems zu nutzen:
// Das braucht keinen Plattformcode und ist für Asset-Ordner mit einigen hundert Dateien billig.
//
// Editoren schreiben Dateien oft in mehreren Schritten. Eine neue oder geänderte Datei meldet
// der Beobachter deshalb erst, wenn sie einen Durchlauf lang gleich geblieben ist; gelöschte
// sofort. Dateien, die auf „.tmp“ oder „~“ enden, und versteckte Einträge (Punkt am Anfang)
// zählen nicht.
class FileWatcher {
public:
    // root muss nicht existieren; dann meldet der Beobachter nichts, bis es ihn gibt.
    explicit FileWatcher(std::string_view root, f64 interval_seconds = 0.25);

    // Ein Durchlauf, wenn seit dem letzten mindestens das Intervall vergangen ist.
    [[nodiscard]] std::vector<FileEvent> poll();
    // Ein Durchlauf sofort. Der erste merkt sich nur den Stand und meldet nichts.
    [[nodiscard]] std::vector<FileEvent> scan();

    [[nodiscard]] const std::string& root() const noexcept {
        return root_;
    }

private:
    struct Stamp {
        u64 size = 0;
        i64 modified = 0;
        friend bool operator==(const Stamp&, const Stamp&) = default;
    };

    std::string root_;
    f64 interval_;
    f64 last_scan_ = -1.0e30;
    bool first_ = true;
    std::map<std::string, Stamp> known_;
    // Neu oder geändert gesehen, aber noch nicht gemeldet.
    std::map<std::string, Stamp> pending_;
};

} // namespace ngine::filesystem
