module;

#include <concepts>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

export module ngine.serialize:json;

import ngine.core;

export namespace ngine::json {

class Value;
struct Member;

using Array = std::vector<Value>;
// Objekte behalten die Reihenfolge ihrer Schlüssel. Das hält Dateien stabil und Diffs klein;
// gesucht wird linear, was für die kleinen Objekte in Szenen schneller ist als eine Hashtabelle.
using Object = std::vector<Member>;

enum class Type : u8 {
    Null,
    Bool,
    // Ganze Zahlen ohne Nachkommastellen und Exponent. Unsigned nur über dem Bereich von i64.
    Int,
    Unsigned,
    Float,
    String,
    Array,
    Object,
};

// Ein JSON-Wert. Wie ein JsonNode in Java-Bibliotheken, aber als Wert: Kopieren kopiert den Baum.
class Value {
public:
    // Alle Konstruktoren, die data_ füllen, stehen in json_value.cpp: Hier ist Member noch
    // unvollständig, und die Variante darf noch nicht über ihre Alternativen nachdenken.
    Value() noexcept;
    Value(std::nullptr_t) noexcept;
    Value(bool value) noexcept;
    template <std::integral T>
        requires(!std::is_same_v<T, bool>)
    Value(T value) noexcept {
        if constexpr (std::is_signed_v<T>) {
            set_int(static_cast<i64>(value));
        } else {
            set_uint(static_cast<u64>(value));
        }
    }
    template <std::floating_point T>
    Value(T value) noexcept {
        set_float(static_cast<f64>(value));
    }
    Value(std::string value) noexcept;
    Value(std::string_view value);
    Value(const char* value);
    // explicit: Sonst wandelt der Compiler beim Vergleichen von Arrays und Objekten still in
    // Value um und landet in einer endlosen Rekursion.
    explicit Value(Array value) noexcept;
    explicit Value(Object value) noexcept;

    [[nodiscard]] Type type() const noexcept {
        return static_cast<Type>(data_.index());
    }
    [[nodiscard]] bool is_null() const noexcept {
        return type() == Type::Null;
    }
    [[nodiscard]] bool is_number() const noexcept {
        return type() == Type::Int || type() == Type::Unsigned || type() == Type::Float;
    }

    [[nodiscard]] std::optional<bool> to_bool() const noexcept;
    // Liefert nichts, wenn der Wert keine Zahl ist oder nicht verlustfrei passt.
    [[nodiscard]] std::optional<i64> to_i64() const noexcept;
    [[nodiscard]] std::optional<u64> to_u64() const noexcept;
    // Jede Zahl, ganze Zahlen auch mit Rundung.
    [[nodiscard]] std::optional<f64> to_f64() const noexcept;

    [[nodiscard]] const std::string* string() const noexcept {
        return std::get_if<std::string>(&data_);
    }
    [[nodiscard]] const Array* array() const noexcept {
        return std::get_if<Array>(&data_);
    }
    [[nodiscard]] Array* array() noexcept {
        return std::get_if<Array>(&data_);
    }
    [[nodiscard]] const Object* object() const noexcept {
        return std::get_if<Object>(&data_);
    }
    [[nodiscard]] Object* object() noexcept {
        return std::get_if<Object>(&data_);
    }

    // Sucht einen Schlüssel in einem Objekt; nullptr, wenn es keiner ist oder er fehlt.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    // Liefert den Wert zu key und legt ihn bei Bedarf an. Aus null wird dabei ein Objekt.
    Value& operator[](std::string_view key);
    // Hängt an ein Array an. Aus null wird dabei ein Array.
    void push_back(Value value);

    friend bool operator==(const Value& a, const Value& b) noexcept;

private:
    void set_int(i64 value) noexcept;
    // Werte bis zum Maximum von i64 landen als Int, damit gleiche Zahlen gleich gespeichert sind.
    void set_uint(u64 value) noexcept;
    void set_float(f64 value) noexcept;

    // Reihenfolge wie in Type.
    std::variant<std::nullptr_t, bool, i64, u64, f64, std::string, Array, Object> data_;
};

struct Member {
    std::string key;
    Value value;

    friend bool operator==(const Member& a, const Member& b) noexcept;
};

struct ParseError {
    std::string message;
    // Ab 1 gezählt.
    usize line = 0;
    usize column = 0;
};

// Liest einen JSON-Text nach RFC 8259. Tiefe Verschachtelung (über 256 Ebenen) gilt als Fehler,
// damit böswillige Dateien den Stack nicht sprengen.
[[nodiscard]] std::expected<Value, ParseError> parse(std::string_view text);

struct WriteOptions {
    // Mit Zeilenumbrüchen und Einrückung, sonst kompakt.
    bool pretty = true;
    u32 indent = 2;
};

// Schreibt einen Wert als JSON. Gleitkommazahlen werden so kurz wie möglich geschrieben und beim
// Lesen bitgenau wiederhergestellt; NaN und Unendlich gibt es in JSON nicht, sie werden zu null.
[[nodiscard]] std::string write(const Value& value, const WriteOptions& options = {});

} // namespace ngine::json
