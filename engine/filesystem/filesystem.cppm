module;

#include <cstddef>
#include <expected>
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
[[nodiscard]] Result<void> create_directories(std::string_view path);
[[nodiscard]] Result<void> remove(std::string_view path);

// Hängt name an path an, mit dem passenden Trenner.
[[nodiscard]] std::string join(std::string_view path, std::string_view name);

// Ordner für Daten des Benutzers (Spielstände, Einstellungen, Szenen) mit einem Unterordner für die
// Anwendung; er wird bei Bedarf angelegt. Linux: $XDG_DATA_HOME bzw. ~/.local/share,
// Windows: Roaming-AppData.
[[nodiscard]] Result<std::string> user_data_dir(std::string_view application);

} // namespace ngine::filesystem
