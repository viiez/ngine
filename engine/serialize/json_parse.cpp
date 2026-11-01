module;

#include <charconv>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

module ngine.serialize;

import ngine.core;

namespace ngine::json {

namespace {

constexpr u32 max_depth = 256;

// Rekursiver Abstieg über den Text. Jede parse_*-Funktion steht vor ihrem Wert und lässt pos_
// hinter ihm stehen; Fehler merkt sich der Parser in error_ und bricht dann überall ab.
class Parser {
public:
    explicit Parser(std::string_view text) noexcept : text_{text} {}

    std::expected<Value, ParseError> run() {
        skip_whitespace();
        Value value = parse_value(0);
        if (!failed_) {
            skip_whitespace();
            if (pos_ != text_.size()) {
                fail("Unerwartete Zeichen nach dem Ende");
            }
        }
        if (failed_) {
            return std::unexpected(std::move(error_));
        }
        return value;
    }

private:
    void fail(std::string_view message) {
        if (failed_) {
            return;
        }
        failed_ = true;
        usize line = 1;
        usize column = 1;
        for (usize i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        error_ = ParseError{.message = std::string{message}, .line = line, .column = column};
    }

    [[nodiscard]] bool at_end() const noexcept {
        return pos_ >= text_.size();
    }
    [[nodiscard]] char peek() const noexcept {
        return at_end() ? '\0' : text_[pos_];
    }

    void skip_whitespace() noexcept {
        while (!at_end()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return;
            }
            ++pos_;
        }
    }

    bool consume(std::string_view word) {
        if (text_.substr(pos_, word.size()) == word) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    Value parse_value(u32 depth) {
        if (depth > max_depth) {
            fail("Zu tief verschachtelt");
            return {};
        }
        switch (peek()) {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"': {
            std::string text;
            parse_string(text);
            return Value{std::move(text)};
        }
        case 't':
            if (consume("true")) {
                return Value{true};
            }
            break;
        case 'f':
            if (consume("false")) {
                return Value{false};
            }
            break;
        case 'n':
            if (consume("null")) {
                return Value{};
            }
            break;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                return parse_number();
            }
            break;
        }
        fail(at_end() ? "Unerwartetes Ende, Wert erwartet" : "Wert erwartet");
        return {};
    }

    Value parse_object(u32 depth) {
        ++pos_; // {
        Object members;
        skip_whitespace();
        if (peek() == '}') {
            ++pos_;
            return Value{std::move(members)};
        }
        while (!failed_) {
            skip_whitespace();
            if (peek() != '"') {
                fail("Schlüssel in Anführungszeichen erwartet");
                break;
            }
            Member member;
            parse_string(member.key);
            skip_whitespace();
            if (peek() != ':') {
                fail("':' erwartet");
                break;
            }
            ++pos_;
            skip_whitespace();
            member.value = parse_value(depth + 1);
            members.push_back(std::move(member));
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                break;
            }
            fail("',' oder '}' erwartet");
        }
        return Value{std::move(members)};
    }

    Value parse_array(u32 depth) {
        ++pos_; // [
        Array elements;
        skip_whitespace();
        if (peek() == ']') {
            ++pos_;
            return Value{std::move(elements)};
        }
        while (!failed_) {
            skip_whitespace();
            elements.push_back(parse_value(depth + 1));
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            fail("',' oder ']' erwartet");
        }
        return Value{std::move(elements)};
    }

    static void append_utf8(std::string& out, u32 code_point) {
        if (code_point < 0x80) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    bool parse_hex4(u32& out) {
        if (pos_ + 4 > text_.size()) {
            fail("Unvollständige \\u-Sequenz");
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<u32>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<u32>(c - 'A' + 10);
            } else {
                fail("Ungültige Hex-Ziffer in \\u-Sequenz");
                return false;
            }
        }
        return true;
    }

    void parse_string(std::string& out) {
        ++pos_; // "
        while (true) {
            if (at_end()) {
                fail("Text ohne schließendes Anführungszeichen");
                return;
            }
            const char c = text_[pos_++];
            if (c == '"') {
                return;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("Steuerzeichen im Text müssen maskiert sein");
                return;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_end()) {
                fail("Unvollständige Escape-Sequenz");
                return;
            }
            const char escape = text_[pos_++];
            switch (escape) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                u32 code_point = 0;
                if (!parse_hex4(code_point)) {
                    return;
                }
                // Zeichen außerhalb der BMP kommen als Paar aus zwei Surrogaten.
                if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                    u32 low = 0;
                    if (!consume("\\u") || !parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        fail("Unvollständiges Surrogat-Paar");
                        return;
                    }
                    code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                    fail("Einzelnes Surrogat");
                    return;
                }
                append_utf8(out, code_point);
                break;
            }
            default: fail("Unbekannte Escape-Sequenz"); return;
            }
        }
    }

    Value parse_number() {
        // Erst die Grammatik prüfen (from_chars ist großzügiger als JSON), dann umwandeln.
        const usize start = pos_;
        bool integral = true;
        if (peek() == '-') {
            ++pos_;
        }
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        } else {
            fail("Ziffer erwartet");
            return {};
        }
        if (peek() == '.') {
            integral = false;
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) {
                fail("Ziffer nach dem Dezimalpunkt erwartet");
                return {};
            }
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            integral = false;
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            if (!(peek() >= '0' && peek() <= '9')) {
                fail("Ziffer im Exponenten erwartet");
                return {};
            }
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }

        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;
        if (integral) {
            i64 value = 0;
            if (const auto result = std::from_chars(first, last, value); result.ec == std::errc{}) {
                return Value{value};
            }
            u64 big = 0;
            if (*first != '-') {
                if (const auto result = std::from_chars(first, last, big);
                    result.ec == std::errc{}) {
                    return Value{big};
                }
            }
            // Zu groß für 64 Bit: als Gleitkommazahl weiter.
        }
        f64 value = 0.0;
        const auto result = std::from_chars(first, last, value);
        if (result.ec == std::errc::result_out_of_range) {
            fail("Zahl außerhalb des Bereichs von f64");
            return {};
        }
        if (result.ec != std::errc{}) {
            fail("Ungültige Zahl");
            return {};
        }
        return Value{value};
    }

    std::string_view text_;
    usize pos_ = 0;
    bool failed_ = false;
    ParseError error_;
};

} // namespace

std::expected<Value, ParseError> parse(std::string_view text) {
    return Parser{text}.run();
}

} // namespace ngine::json
