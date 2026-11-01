module;

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <system_error>

module ngine.serialize;

import ngine.core;

namespace ngine::json {

namespace {

void write_string(std::string& out, const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                const auto code = static_cast<unsigned char>(c);
                out += "\\u00";
                out.push_back(hex[code >> 4]);
                out.push_back(hex[code & 0xF]);
            } else {
                // UTF-8 bleibt, wie es ist.
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

template <typename T>
void write_number(std::string& out, T value) {
    char buffer[32];
    // Ohne Format und Genauigkeit: die kürzeste Darstellung, die beim Lesen denselben Wert ergibt.
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

class Writer {
public:
    Writer(std::string& out, const WriteOptions& options) noexcept : out_{out}, options_{options} {}

    void write(const Value& value, u32 depth) {
        switch (value.type()) {
        case Type::Null: out_ += "null"; break;
        case Type::Bool: out_ += *value.to_bool() ? "true" : "false"; break;
        case Type::Int: write_number(out_, *value.to_i64()); break;
        case Type::Unsigned: write_number(out_, *value.to_u64()); break;
        case Type::Float: {
            const f64 number = *value.to_f64();
            if (std::isfinite(number)) {
                const usize start = out_.size();
                write_number(out_, number);
                // Ohne Punkt und Exponent läse man eine ganze Zahl zurück; aus -0.0 würde 0.
                if (out_.find_first_of(".eE", start) == std::string::npos) {
                    out_ += ".0";
                }
            } else {
                out_ += "null";
            }
            break;
        }
        case Type::String: write_string(out_, *value.string()); break;
        case Type::Array: write_array(*value.array(), depth); break;
        case Type::Object: write_object(*value.object(), depth); break;
        }
    }

private:
    void newline(u32 depth) {
        if (options_.pretty) {
            out_.push_back('\n');
            out_.append(usize{depth} * options_.indent, ' ');
        }
    }

    void write_array(const Array& elements, u32 depth) {
        if (elements.empty()) {
            out_ += "[]";
            return;
        }
        out_.push_back('[');
        for (usize i = 0; i < elements.size(); ++i) {
            if (i > 0) {
                out_.push_back(',');
            }
            newline(depth + 1);
            write(elements[i], depth + 1);
        }
        newline(depth);
        out_.push_back(']');
    }

    void write_object(const Object& members, u32 depth) {
        if (members.empty()) {
            out_ += "{}";
            return;
        }
        out_.push_back('{');
        for (usize i = 0; i < members.size(); ++i) {
            if (i > 0) {
                out_.push_back(',');
            }
            newline(depth + 1);
            write_string(out_, members[i].key);
            out_ += options_.pretty ? ": " : ":";
            write(members[i].value, depth + 1);
        }
        newline(depth);
        out_.push_back('}');
    }

    std::string& out_;
    const WriteOptions& options_;
};

} // namespace

std::string write(const Value& value, const WriteOptions& options) {
    std::string out;
    Writer{out, options}.write(value, 0);
    if (options.pretty) {
        out.push_back('\n');
    }
    return out;
}

} // namespace ngine::json
