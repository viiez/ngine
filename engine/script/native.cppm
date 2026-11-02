module;

#include <string>
#include <vector>

// Plattformcode für das Laden von hostfxr: dynamische Bibliotheken und die üblichen Orte einer
// .NET-Installation. Umsetzung in native_linux.cpp und native_windows.cpp. Interne Partition, nur
// mit UTF-8-Text statt std::filesystem::path (siehe Erkenntnisse aus A4).
module ngine.script:native;

namespace ngine::script {

// Lädt eine Bibliothek; nullptr bei Fehler, Meldung in error.
void* load_library(const std::string& path, std::string& error);
void* find_symbol(void* library, const char* name);

// Dateiname von hostfxr auf dieser Plattform.
const char* hostfxr_file_name();
// Mögliche Installationsorte von .NET in der Reihenfolge, in der sie zu prüfen sind: zuerst
// DOTNET_ROOT, dann die üblichen Orte des Systems.
std::vector<std::string> dotnet_roots();

} // namespace ngine::script
