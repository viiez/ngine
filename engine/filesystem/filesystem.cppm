module;

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Dateizugriff der Engine (PLAT-04). Läuft ohne Fenster und ohne SDL3, damit auch der dedizierte
// Server Dateien lesen und schreiben kann.
//
// Pfade sind UTF-8-Text. Intern werden sie in std::filesystem::path umgewandelt; so gibt es unter
// Windows keine Probleme mit der Codepage, und die Schnittstelle bleibt frei von <filesystem>.
export module ngine.filesystem;

export import :watcher;
import ngine.core;

export namespace ngine::filesystem {

// Fehler enthalten eine lesbare Meldung mit Pfad und Ursache.
template <typename T>
using Result = std::expected<T, std::string>;

[[nodiscard]] Result<std::vector<std::byte>> read_file(std::string_view path);
[[nodiscard]] Result<std::string> read_text(std::string_view path);

// Schreibt erst in eine temporäre Datei daneben und benennt sie dann um. Ein Absturz mitten im
// Schreiben hinterlässt so die alte Datei statt einer halben. Fehlende Ordner werden angelegt.
[[nodiscard]] Result<void> write_file(std::string_view path, std::span<const std::byte> data);
[[nodiscard]] Result<void> write_text(std::string_view path, std::string_view text);

[[nodiscard]] bool exists(std::string_view path) noexcept;
[[nodiscard]] bool is_directory(std::string_view path) noexcept;
[[nodiscard]] Result<void> create_directories(std::string_view path);
[[nodiscard]] Result<void> remove(std::string_view path);
// Benennt um oder verschiebt; ein vorhandenes Ziel wird ersetzt.
[[nodiscard]] Result<void> rename(std::string_view from, std::string_view to);

// Hängt name an path an, mit dem passenden Trenner.
[[nodiscard]] std::string join(std::string_view path, std::string_view name);

// --- Pfade (R2.4) ---
// Absoluter Pfad ohne „.“ und „..“, mit „/“ als Trenner auf allen Plattformen. So lassen sich
// Pfade aus verschiedenen Quellen (Asset-Datenbank, glTF, Dateibeobachter) vergleichen.
[[nodiscard]] std::string normalize(std::string_view path);
// path relativ zu base mit „/“ als Trenner, z. B. „modelle/kiste.gltf“; leer, wenn path nicht
// unter base liegt.
[[nodiscard]] std::string relative(std::string_view path, std::string_view base);
// Ordner, Dateiname und Endung (klein geschrieben, mit Punkt, z. B. „.gltf“) eines Pfads.
[[nodiscard]] std::string parent_path(std::string_view path);
[[nodiscard]] std::string file_name(std::string_view path);
[[nodiscard]] std::string extension(std::string_view path);

// Eintrag eines Ordners.
struct FileEntry {
    // Relativ zum aufgelisteten Ordner, mit „/“ als Trenner.
    std::string path;
    bool directory = false;
    u64 size = 0;
    // Zeit der letzten Änderung in Ticks der Dateiuhr; nur zum Vergleichen gedacht.
    i64 modified = 0;
};

// Einträge eines Ordners, sortiert nach Pfad. recursive: auch alle Unterordner; Ordner und
// Dateien, deren Name mit einem Punkt beginnt (.git), fehlen dann.
[[nodiscard]] Result<std::vector<FileEntry>> list_directory(std::string_view path,
                                                            bool recursive = false);
// Größe und Änderungszeit einer Datei, nichts wenn es sie nicht gibt.
[[nodiscard]] std::optional<FileEntry> status(std::string_view path);

// Ordner der laufenden ausführbaren Datei (TD-12): Daten, die neben dem Programm liegen
// (C#-Assemblies), werden relativ dazu gesucht.
[[nodiscard]] Result<std::string> executable_dir();

// Ordner für Daten des Benutzers (Spielstände, Einstellungen, Szenen) mit einem Unterordner für die
// Anwendung; er wird bei Bedarf angelegt. Linux: $XDG_DATA_HOME bzw. ~/.local/share,
// Windows: Roaming-AppData.
[[nodiscard]] Result<std::string> user_data_dir(std::string_view application);

} // namespace ngine::filesystem
