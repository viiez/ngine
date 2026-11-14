module;

#include <string>
#include <string_view>
#include <vector>

// C#-Bindings aus der Reflection (SCR-02, R4.4): Für jeden reflektierten Struct entsteht ein
// C#-Struct mit expliziter Anordnung (Offsets und Größe aus der TypeInfo), für jedes Enum ein
// C#-Enum mit gleichem Grundtyp. ngine_bindgen schreibt das Ergebnis beim Bauen nach
// Components.g.cs; NGine.Scripting baut die Datei mit.
export module ngine.script:bindings;

import ngine.core;
import ngine.reflect;

export namespace ngine::script {

struct BindingOptions {
    std::string name_space = "NGine";
    // Typen, die schon in einer anderen erzeugten Datei stehen (z. B. die der Engine, wenn eine
    // Anwendung ihre eigenen Komponenten erzeugt). Gleicher Name: wird übersprungen.
    const reflect::TypeRegistry* base = nullptr;
    // Namen, die schon in NGine.Scripting stehen (zur Laufzeit von C# gemeldet).
    std::vector<std::string> skip;
    // GeneratedBindings.Describe für die Prüfung der Anordnung; nur in NGine.Scripting.
    bool layout_report = true;
};

// Typen, die NGine.Scripting von Hand mitbringt (Mathe-Typen mit Operatoren). Sie werden nicht
// erzeugt, ihre Anordnung wird aber mit geprüft.
[[nodiscard]] bool provided_by_hand(std::string_view type_name) noexcept;

// Name eines Feldes in C#: snake_case wird zu PascalCase.
[[nodiscard]] std::string csharp_field_name(std::string_view name);

// Ob ein Feld in C# im Struct steht. Texte (std::string) fehlen, ihr Platz bleibt frei.
[[nodiscard]] bool csharp_visible(const reflect::Field& field) noexcept;

// C#-Quelltext für alle Structs und Enums der Registry (ohne Skript-Typen und ausgemusterte).
[[nodiscard]] std::string generate_bindings(const reflect::TypeRegistry& types,
                                            const BindingOptions& options = {});

} // namespace ngine::script
