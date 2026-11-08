module;

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Übersetzt Slang-Shader zur Laufzeit in SPIR-V (R2.4, ADR 0034), für Hot Reload der Shader im
// Editor. Beim Bauen erledigt das weiterhin slangc (ngine_compile_shader()); dieses Modul ist nur
// für Werkzeuge gedacht, Spiel und Server linken es nicht. Die Slang-Bibliothek kennt nur
// compiler.cpp.
export module ngine.shader_compiler;

import ngine.core;
import ngine.rhi;

export namespace ngine::shader_compiler {

template <typename T>
using Result = std::expected<T, std::string>;

class Compiler {
public:
    // Lädt die Slang-Bibliothek (dauert einige hundert Millisekunden).
    [[nodiscard]] static Result<std::unique_ptr<Compiler>> create();
    ~Compiler();
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    // Übersetzt den Einstiegspunkt entry der Datei path (UTF-8) für stage, mit denselben Optionen
    // wie beim Bauen. #include sucht im Ordner der Datei. Jeder Aufruf liest die Dateien neu.
    // Fehler enthalten die Meldungen von Slang mit Datei und Zeile.
    [[nodiscard]] Result<std::vector<u32>> compile(std::string_view path, std::string_view entry,
                                                   rhi::ShaderStage stage);

private:
    struct Impl;
    explicit Compiler(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::shader_compiler
