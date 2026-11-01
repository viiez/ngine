module;

#include <dlfcn.h>

#include <cstdlib>
#include <string>
#include <vector>

module ngine.script;

import :native;

namespace ngine::script {

void* load_library(const std::string& path, std::string& error) {
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char* message = dlerror();
        error = message != nullptr ? message : "dlopen fehlgeschlagen";
    }
    return library;
}

void* find_symbol(void* library, const char* name) {
    return dlsym(library, name);
}

const char* hostfxr_file_name() {
    return "libhostfxr.so";
}

std::vector<std::string> dotnet_roots() {
    std::vector<std::string> roots;
    if (const char* root = std::getenv("DOTNET_ROOT"); root != nullptr && *root != '\0') {
        roots.emplace_back(root);
    }
    // Pakete von Ubuntu legen .NET nach /usr/lib/dotnet, die von Microsoft nach /usr/share/dotnet,
    // das Installationsskript nach ~/.dotnet.
    for (const char* path : {"/usr/lib/dotnet", "/usr/share/dotnet", "/usr/lib64/dotnet"}) {
        roots.emplace_back(path);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        roots.push_back(std::string{home} + "/.dotnet");
    }
    return roots;
}

} // namespace ngine::script
