module;

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>

module ngine.script;

import ngine.core;
import ngine.reflect;

namespace ngine::script {

namespace {

constexpr std::array<std::string_view, 5> hand_written{"Vec2", "Vec3", "Vec4", "DVec3", "Quat"};

// Namen, die NGine.Scripting schon vergibt; ein reflektierter Typ darf sie nicht tragen.
constexpr std::array<std::string_view, 12> reserved{
    "Entity", "World", "Log",      "Host",    "Input",      "Physics",
    "Time",   "Frame", "Position", "IScript", "IComponent", "NativeApi"};

// C#-Typ eines Feldes; leer, wenn das Feld in C# fehlt.
std::string csharp_type(const reflect::TypeInfo& type) {
    switch (type.kind) {
    case reflect::Kind::Bool: return "bool";
    case reflect::Kind::Int:
        switch (type.size) {
        case 1: return "sbyte";
        case 2: return "short";
        case 4: return "int";
        default: return "long";
        }
    case reflect::Kind::UInt:
        switch (type.size) {
        case 1: return "byte";
        case 2: return "ushort";
        case 4: return "uint";
        default: return "ulong";
        }
    case reflect::Kind::Float: return type.size == 4 ? "float" : "double";
    case reflect::Kind::String: return {};
    case reflect::Kind::Enum:
    case reflect::Kind::Struct: return type.name;
    }
    return {};
}

// Grundtyp eines Enums nach seiner Größe. Die Reflection kennt das Vorzeichen nicht; Enums der
// Engine mit festem Grundtyp sind vorzeichenlos (u8), solche ohne sind int.
std::string_view enum_base(usize size) {
    switch (size) {
    case 1: return "byte";
    case 2: return "ushort";
    case 8: return "long";
    default: return "int";
    }
}

bool is_reserved(std::string_view name) {
    for (const std::string_view r : reserved) {
        if (r == name) {
            return true;
        }
    }
    return false;
}

// Feldname, der in C# nicht mit dem Struct oder dessen statischen Mitgliedern kollidiert.
std::string member_name(const reflect::TypeInfo& owner, const reflect::Field& field) {
    std::string name = csharp_field_name(field.name);
    if (name == owner.name || name == "TypeName" || name == "Trivial") {
        name += '_';
    }
    return name;
}

void write_enum(std::string& out, const reflect::TypeInfo& type) {
    out += std::format("public enum {} : {}\n{{\n", type.name, enum_base(type.size));
    for (const reflect::Enumerator& value : type.enumerators) {
        out += std::format("    {} = {},\n", value.name, value.value);
    }
    out += "}\n\n";
}

template <typename T>
T load(const std::byte* data) {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

// Wert eines Feldes als C#-Ausdruck; leer bei 0 (C# setzt Felder ohnehin auf 0).
std::string literal(const reflect::TypeInfo& type, const std::byte* data) {
    const auto integer = [&]() -> std::int64_t {
        switch (type.size) {
        case 1:
            return type.kind == reflect::Kind::Int ? load<std::int8_t>(data)
                                                   : load<std::uint8_t>(data);
        case 2:
            return type.kind == reflect::Kind::Int ? load<std::int16_t>(data)
                                                   : load<std::uint16_t>(data);
        case 4:
            return type.kind == reflect::Kind::Int ? load<std::int32_t>(data)
                                                   : load<std::uint32_t>(data);
        default: return load<std::int64_t>(data);
        }
    };
    switch (type.kind) {
    case reflect::Kind::Bool: return load<bool>(data) ? "true" : "";
    case reflect::Kind::Int: {
        const std::int64_t value = integer();
        return value != 0 ? std::format("({}){}", csharp_type(type), value) : "";
    }
    case reflect::Kind::UInt: {
        const auto value =
            type.size == 8 ? load<std::uint64_t>(data) : static_cast<std::uint64_t>(integer());
        return value != 0 ? std::format("({}){}UL", csharp_type(type), value) : "";
    }
    case reflect::Kind::Enum: {
        const std::int64_t value = integer();
        return value != 0 ? std::format("({}){}", type.name, value) : "";
    }
    case reflect::Kind::Float: {
        const f64 value = type.size == 4 ? static_cast<f64>(load<f32>(data)) : load<f64>(data);
        if (value == 0.0) {
            return "";
        }
        const std::string_view name = type.size == 4 ? "float" : "double";
        if (std::isinf(value)) {
            return std::format("{}.{}", name, value > 0 ? "PositiveInfinity" : "NegativeInfinity");
        }
        if (std::isnan(value)) {
            return std::format("{}.NaN", name);
        }
        return type.size == 4 ? std::format("{}f", load<f32>(data)) : std::format("{}d", value);
    }
    default: return "";
    }
}

// Zuweisungen für die Standardwerte aus C++ (Konstruktor ohne Parameter), Feld für Feld bis zu
// den Zahlen, damit verschachtelte Structs keine eigenen Konstruktoren brauchen.
void write_defaults(std::string& out, const reflect::TypeInfo& type, const std::byte* data,
                    const std::string& path) {
    for (const reflect::Field& field : type.fields) {
        if (!csharp_visible(field)) {
            continue;
        }
        const std::string name = path + (provided_by_hand(type.name) ? csharp_field_name(field.name)
                                                                     : member_name(type, field));
        if (field.type->kind == reflect::Kind::Struct) {
            write_defaults(out, *field.type, data + field.offset, name + ".");
            continue;
        }
        if (const std::string value = literal(*field.type, data + field.offset); !value.empty()) {
            out += std::format("        {} = {};\n", name, value);
        }
    }
}

void write_struct(std::string& out, const reflect::TypeInfo& type) {
    out += std::format("[StructLayout(LayoutKind.Explicit, Size = {})]\n", type.size);
    out += std::format("public struct {} : IComponent\n{{\n", type.name);
    out += std::format("    public static string TypeName => \"{}\";\n", type.name);
    out += std::format("    public static bool Trivial => {};\n", type.trivial ? "true" : "false");
    // Standardwerte wie in C++ (new T(); default(T) bleibt überall 0).
    {
        auto storage = std::make_unique<std::byte[]>(type.size + type.alignment);
        void* raw = storage.get();
        usize space = type.size + type.alignment;
        void* aligned = std::align(type.alignment, type.size, raw, space);
        type.construct(aligned);
        std::string defaults;
        write_defaults(defaults, type, static_cast<const std::byte*>(aligned), "");
        type.destroy(aligned);
        out += std::format("    public {}()\n    {{\n{}    }}\n", type.name, defaults);
    }
    for (const reflect::Field& field : type.fields) {
        if (!csharp_visible(field)) {
            out += std::format("    // {} (Text, über Zugriffsfunktionen) bei Offset {}\n",
                               field.name, field.offset);
            continue;
        }
        out += std::format("    [FieldOffset({})] public {} {};\n", field.offset,
                           csharp_type(*field.type), member_name(type, field));
    }
    out += "}\n\n";
}

void write_layout(std::string& out, const reflect::TypeInfo& type) {
    bool any_field = false;
    for (const reflect::Field& field : type.fields) {
        any_field = any_field || csharp_visible(field);
    }
    out += "        {\n";
    if (any_field) {
        out += std::format("            {} v = default;\n", type.name);
    }
    out += std::format("            w.Type(\"{}\", sizeof({}));\n", type.name, type.name);
    for (const reflect::Field& field : type.fields) {
        if (!csharp_visible(field)) {
            continue;
        }
        const std::string name =
            provided_by_hand(type.name) ? csharp_field_name(field.name) : member_name(type, field);
        out += std::format("            w.Field(\"{}\", (byte*)&v.{} - (byte*)&v);\n", field.name,
                           name);
    }
    out += "        }\n";
}

} // namespace

bool provided_by_hand(std::string_view type_name) noexcept {
    for (const std::string_view name : hand_written) {
        if (name == type_name) {
            return true;
        }
    }
    return false;
}

std::string csharp_field_name(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    bool upper = true;
    for (const char c : name) {
        if (c == '_') {
            upper = true;
            continue;
        }
        if (upper && c >= 'a' && c <= 'z') {
            result += static_cast<char>(c - 'a' + 'A');
        } else {
            result += c;
        }
        upper = false;
    }
    return result;
}

bool csharp_visible(const reflect::Field& field) noexcept {
    return field.type != nullptr && field.type->kind != reflect::Kind::String;
}

std::string generate_bindings(const reflect::TypeRegistry& types, const BindingOptions& options) {
    std::string out;
    out += "// <auto-generated>\n";
    out += "// Erzeugt von ngine_bindgen aus der Reflection der Engine (R4.4). Nicht von Hand\n";
    out += "// ändern; die Datei entsteht bei jedem Bauen neu.\n";
    out += "// </auto-generated>\n\n";
    out += "using System.Runtime.InteropServices;\n";
    if (options.name_space != "NGine") {
        out += "using NGine;\n";
    }
    out += "\n";
    out += std::format("namespace {};\n\n", options.name_space);

    const auto wanted = [&](const reflect::TypeInfo& type) {
        if (type.kind != reflect::Kind::Struct && type.kind != reflect::Kind::Enum) {
            return false;
        }
        if (options.base != nullptr && options.base->find(type.name) != nullptr) {
            return false;
        }
        verify(
            !is_reserved(type.name),
            std::format("Bindings: Der Typ {} heißt wie ein Typ von NGine.Scripting", type.name));
        return true;
    };

    for (const reflect::TypeInfo* type : types.types()) {
        if (!wanted(*type)) {
            continue;
        }
        if (type->kind == reflect::Kind::Enum) {
            write_enum(out, *type);
        } else if (!provided_by_hand(type->name)) {
            write_struct(out, *type);
        }
    }

    // Anordnung, wie C# sie sieht; die Engine vergleicht sie beim Start mit der Reflection.
    out += "internal static unsafe partial class GeneratedBindings\n{\n";
    out += "    static partial void Describe(LayoutWriter w)\n    {\n";
    for (const reflect::TypeInfo* type : types.types()) {
        if (wanted(*type) && type->kind == reflect::Kind::Struct) {
            write_layout(out, *type);
        }
    }
    out += "    }\n}\n";
    return out;
}

} // namespace ngine::script
