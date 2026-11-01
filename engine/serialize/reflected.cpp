module;

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

module ngine.serialize;

import ngine.core;
import ngine.reflect;

namespace ngine::serialize {

namespace {

using reflect::Kind;
using reflect::TypeInfo;

const std::byte* at(const void* object, usize offset) noexcept {
    return static_cast<const std::byte*>(object) + offset;
}
std::byte* at(void* object, usize offset) noexcept {
    return static_cast<std::byte*>(object) + offset;
}

// Liest bzw. schreibt eine Ganzzahl beliebiger Breite. memcpy vermeidet Aliasing-Probleme.
i64 load_int(const void* source, usize size) noexcept {
    switch (size) {
    case 1: {
        i8 v;
        std::memcpy(&v, source, 1);
        return v;
    }
    case 2: {
        i16 v;
        std::memcpy(&v, source, 2);
        return v;
    }
    case 4: {
        i32 v;
        std::memcpy(&v, source, 4);
        return v;
    }
    default: {
        i64 v;
        std::memcpy(&v, source, 8);
        return v;
    }
    }
}

u64 load_uint(const void* source, usize size) noexcept {
    u64 v = 0;
    switch (size) {
    case 1: {
        u8 x;
        std::memcpy(&x, source, 1);
        v = x;
        break;
    }
    case 2: {
        u16 x;
        std::memcpy(&x, source, 2);
        v = x;
        break;
    }
    case 4: {
        u32 x;
        std::memcpy(&x, source, 4);
        v = x;
        break;
    }
    default: std::memcpy(&v, source, 8); break;
    }
    return v;
}

template <typename T>
void store(void* target, T value) noexcept {
    std::memcpy(target, &value, sizeof(T));
}

bool store_int(void* target, usize size, i64 value) noexcept {
    switch (size) {
    case 1:
        if (value < std::numeric_limits<i8>::min() || value > std::numeric_limits<i8>::max()) {
            return false;
        }
        store(target, static_cast<i8>(value));
        return true;
    case 2:
        if (value < std::numeric_limits<i16>::min() || value > std::numeric_limits<i16>::max()) {
            return false;
        }
        store(target, static_cast<i16>(value));
        return true;
    case 4:
        if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max()) {
            return false;
        }
        store(target, static_cast<i32>(value));
        return true;
    default: store(target, value); return true;
    }
}

bool store_uint(void* target, usize size, u64 value) noexcept {
    switch (size) {
    case 1:
        if (value > std::numeric_limits<u8>::max()) {
            return false;
        }
        store(target, static_cast<u8>(value));
        return true;
    case 2:
        if (value > std::numeric_limits<u16>::max()) {
            return false;
        }
        store(target, static_cast<u16>(value));
        return true;
    case 4:
        if (value > std::numeric_limits<u32>::max()) {
            return false;
        }
        store(target, static_cast<u32>(value));
        return true;
    default: store(target, value); return true;
    }
}

f64 load_float(const void* source, usize size) noexcept {
    if (size == sizeof(f32)) {
        f32 v;
        std::memcpy(&v, source, sizeof(v));
        // 0.1f ist als f64 0.10000000149011612. Über die kürzeste Darstellung als f32 wird daraus
        // 0.1; zurück nach f32 gerundet ergibt das wieder genau v. So bleibt die Datei lesbar.
        char buffer[32];
        const auto written = std::to_chars(buffer, buffer + sizeof(buffer), v);
        f64 shortest = 0.0;
        if (written.ec == std::errc{} &&
            std::from_chars(buffer, written.ptr, shortest).ec == std::errc{}) {
            return shortest;
        }
        return v;
    }
    f64 v;
    std::memcpy(&v, source, sizeof(v));
    return v;
}

void store_float(void* target, usize size, f64 value) noexcept {
    if (size == sizeof(f32)) {
        store(target, static_cast<f32>(value));
    } else {
        store(target, value);
    }
}

std::string child_path(std::string_view path, std::string_view field) {
    return path.empty() ? std::string{field} : std::format("{}.{}", path, field);
}

Result<void> mismatch(std::string_view path, const TypeInfo& type, std::string_view expected) {
    return std::unexpected(std::format("'{}' ({}): {} erwartet", path.empty() ? type.name : path,
                                       type.name, expected));
}

Result<void> read_json(const json::Value& value, void* object, const TypeInfo& type,
                       const std::string& path) {
    switch (type.kind) {
    case Kind::Bool: {
        const auto flag = value.to_bool();
        if (!flag) {
            return mismatch(path, type, "true oder false");
        }
        store(object, *flag);
        return {};
    }
    case Kind::Int: {
        const auto number = value.to_i64();
        if (!number || !store_int(object, type.size, *number)) {
            return mismatch(path, type, "ganze Zahl im Wertebereich");
        }
        return {};
    }
    case Kind::UInt: {
        const auto number = value.to_u64();
        if (!number || !store_uint(object, type.size, *number)) {
            return mismatch(path, type, "nicht negative ganze Zahl im Wertebereich");
        }
        return {};
    }
    case Kind::Float: {
        // null steht für NaN und Unendlich, die JSON nicht kennt.
        if (value.is_null()) {
            store_float(object, type.size, std::numeric_limits<f64>::quiet_NaN());
            return {};
        }
        const auto number = value.to_f64();
        if (!number) {
            return mismatch(path, type, "Zahl");
        }
        store_float(object, type.size, *number);
        return {};
    }
    case Kind::String: {
        const std::string* text = value.string();
        if (text == nullptr) {
            return mismatch(path, type, "Text");
        }
        *static_cast<std::string*>(object) = *text;
        return {};
    }
    case Kind::Struct: {
        if (value.object() == nullptr) {
            return mismatch(path, type, "Objekt");
        }
        for (const reflect::Field& field : type.fields) {
            const json::Value* member = value.find(field.name);
            if (member == nullptr) {
                continue;
            }
            auto result = read_json(*member, at(object, field.offset), *field.type,
                                    child_path(path, field.name));
            if (!result) {
                return result;
            }
        }
        return {};
    }
    }
    return std::unexpected(std::string{"Unbekannte Typart"});
}

Result<void> truncated(const TypeInfo& type) {
    return std::unexpected(std::format("Binärdaten zu kurz oder ungültig bei '{}'", type.name));
}

} // namespace

json::Value to_json(const void* object, const TypeInfo& type) {
    switch (type.kind) {
    case Kind::Bool: {
        bool flag = false;
        std::memcpy(&flag, object, sizeof(flag));
        return json::Value{flag};
    }
    case Kind::Int: return json::Value{load_int(object, type.size)};
    case Kind::UInt: return json::Value{load_uint(object, type.size)};
    case Kind::Float: return json::Value{load_float(object, type.size)};
    case Kind::String: return json::Value{*static_cast<const std::string*>(object)};
    case Kind::Struct: {
        json::Object members;
        members.reserve(type.fields.size());
        for (const reflect::Field& field : type.fields) {
            members.push_back(json::Member{
                .key = field.name,
                .value = to_json(at(object, field.offset), *field.type),
            });
        }
        return json::Value{std::move(members)};
    }
    }
    return {};
}

Result<void> from_json(const json::Value& value, void* object, const TypeInfo& type) {
    return read_json(value, object, type, {});
}

void write_binary(BinaryWriter& writer, const void* object, const TypeInfo& type) {
    switch (type.kind) {
    case Kind::Bool: {
        bool flag = false;
        std::memcpy(&flag, object, sizeof(flag));
        writer.write(flag);
        return;
    }
    case Kind::Int:
    case Kind::UInt:
    case Kind::Float: writer.write_scalar(object, type.size); return;
    case Kind::String: writer.write_string(*static_cast<const std::string*>(object)); return;
    case Kind::Struct:
        for (const reflect::Field& field : type.fields) {
            write_binary(writer, at(object, field.offset), *field.type);
        }
        return;
    }
}

Result<void> read_binary(BinaryReader& reader, void* object, const TypeInfo& type) {
    switch (type.kind) {
    case Kind::Bool: {
        bool flag = false;
        if (!reader.read(flag)) {
            return truncated(type);
        }
        store(object, flag);
        return {};
    }
    case Kind::Int:
    case Kind::UInt:
    case Kind::Float:
        if (!reader.read_scalar(object, type.size)) {
            return truncated(type);
        }
        return {};
    case Kind::String:
        if (!reader.read_string(*static_cast<std::string*>(object))) {
            return truncated(type);
        }
        return {};
    case Kind::Struct:
        for (const reflect::Field& field : type.fields) {
            auto result = read_binary(reader, at(object, field.offset), *field.type);
            if (!result) {
                return result;
            }
        }
        return {};
    }
    return truncated(type);
}

} // namespace ngine::serialize
