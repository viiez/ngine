module;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

module ngine.serialize;

import ngine.core;

namespace ngine::json {

Value::Value() noexcept = default;
Value::Value(std::nullptr_t) noexcept {}
Value::Value(bool value) noexcept : data_{value} {}
Value::Value(std::string value) noexcept : data_{std::move(value)} {}
Value::Value(std::string_view value) : data_{std::string{value}} {}
Value::Value(const char* value) : data_{std::string{value}} {}
Value::Value(Array value) noexcept : data_{std::move(value)} {}
Value::Value(Object value) noexcept : data_{std::move(value)} {}

void Value::set_int(i64 value) noexcept {
    data_ = value;
}

void Value::set_uint(u64 value) noexcept {
    if (value <= static_cast<u64>(std::numeric_limits<i64>::max())) {
        data_ = static_cast<i64>(value);
    } else {
        data_ = value;
    }
}

void Value::set_float(f64 value) noexcept {
    data_ = value;
}

std::optional<bool> Value::to_bool() const noexcept {
    if (const bool* value = std::get_if<bool>(&data_)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<i64> Value::to_i64() const noexcept {
    switch (type()) {
    case Type::Int: return std::get<i64>(data_);
    case Type::Float: {
        // 3.0 ist eine ganze Zahl, 3.5 nicht. Der Bereich von i64 endet bei 2^63.
        const f64 value = std::get<f64>(data_);
        constexpr f64 limit = 9223372036854775808.0;
        if (std::trunc(value) == value && value >= -limit && value < limit) {
            return static_cast<i64>(value);
        }
        return std::nullopt;
    }
    default: return std::nullopt;
    }
}

std::optional<u64> Value::to_u64() const noexcept {
    switch (type()) {
    case Type::Int: {
        const i64 value = std::get<i64>(data_);
        return value >= 0 ? std::optional<u64>{static_cast<u64>(value)} : std::nullopt;
    }
    case Type::Unsigned: return std::get<u64>(data_);
    case Type::Float: {
        const f64 value = std::get<f64>(data_);
        constexpr f64 limit = 18446744073709551616.0;
        if (std::trunc(value) == value && value >= 0.0 && value < limit) {
            return static_cast<u64>(value);
        }
        return std::nullopt;
    }
    default: return std::nullopt;
    }
}

std::optional<f64> Value::to_f64() const noexcept {
    switch (type()) {
    case Type::Int: return static_cast<f64>(std::get<i64>(data_));
    case Type::Unsigned: return static_cast<f64>(std::get<u64>(data_));
    case Type::Float: return std::get<f64>(data_);
    default: return std::nullopt;
    }
}

const Value* Value::find(std::string_view key) const noexcept {
    if (const Object* members = object()) {
        for (const Member& member : *members) {
            if (member.key == key) {
                return &member.value;
            }
        }
    }
    return nullptr;
}

Value& Value::operator[](std::string_view key) {
    if (is_null()) {
        data_ = Object{};
    }
    Object* members = object();
    verify(members != nullptr, "JSON: operator[] auf einem Wert, der kein Objekt ist");
    for (Member& member : *members) {
        if (member.key == key) {
            return member.value;
        }
    }
    return members->emplace_back(Member{.key = std::string{key}, .value = {}}).value;
}

void Value::push_back(Value value) {
    if (is_null()) {
        data_ = Array{};
    }
    Array* elements = array();
    verify(elements != nullptr, "JSON: push_back auf einem Wert, der kein Array ist");
    elements->push_back(std::move(value));
}

bool operator==(const Value& a, const Value& b) noexcept {
    if (a.type() != b.type()) {
        return false;
    }
    switch (a.type()) {
    case Type::Null: return true;
    case Type::Bool: return std::get<bool>(a.data_) == std::get<bool>(b.data_);
    case Type::Int: return std::get<i64>(a.data_) == std::get<i64>(b.data_);
    case Type::Unsigned: return std::get<u64>(a.data_) == std::get<u64>(b.data_);
    case Type::Float: return std::get<f64>(a.data_) == std::get<f64>(b.data_);
    case Type::String: return std::get<std::string>(a.data_) == std::get<std::string>(b.data_);
    case Type::Array: {
        const Array& x = std::get<Array>(a.data_);
        const Array& y = std::get<Array>(b.data_);
        return std::ranges::equal(x, y, [](const Value& l, const Value& r) { return l == r; });
    }
    case Type::Object: {
        const Object& x = std::get<Object>(a.data_);
        const Object& y = std::get<Object>(b.data_);
        return std::ranges::equal(x, y, [](const Member& l, const Member& r) { return l == r; });
    }
    }
    return false;
}

bool operator==(const Member& a, const Member& b) noexcept {
    return a.key == b.key && a.value == b.value;
}

} // namespace ngine::json
