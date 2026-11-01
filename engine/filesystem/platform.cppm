module;

#include <expected>
#include <string>

// Interne Partition: was jede Plattform selbst beantworten muss. Die Umsetzung steht in
// user_dir_linux.cpp bzw. user_dir_windows.cpp.
module ngine.filesystem:platform;

namespace ngine::filesystem {

// Basisordner für Benutzerdaten (UTF-8), ohne Unterordner der Anwendung. Ohne <filesystem> in
// dieser Schnittstelle: Clang 20 verschluckt sich sonst an std::format in filesystem.cpp.
std::expected<std::string, std::string> platform_user_data_dir();

} // namespace ngine::filesystem
