module;

#include <expected>
#include <string>

// Interne Partition: was jede Plattform selbst beantworten muss. Die Umsetzung steht in
// platform_linux.cpp bzw. platform_windows.cpp.
module ngine.filesystem:platform;

namespace ngine::filesystem {

// Basisordner für Benutzerdaten (UTF-8), ohne Unterordner der Anwendung. Ohne <filesystem> in
// dieser Schnittstelle: Clang 20 verschluckt sich sonst an std::format in filesystem.cpp.
std::expected<std::string, std::string> platform_user_data_dir();

// Vollständiger Pfad der laufenden ausführbaren Datei (UTF-8).
std::expected<std::string, std::string> platform_executable_path();

} // namespace ngine::filesystem
