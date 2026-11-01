module;

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module ngine.serialize:binary;

import ngine.core;

export namespace ngine::serialize {

// Kopiert size Bytes eines Zahlenwerts in Little Endian (auf Big-Endian-Systemen umgedreht).
inline void copy_little_endian(std::byte* target, const std::byte* source, usize size) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(target, source, size);
    } else {
        for (usize i = 0; i < size; ++i) {
            target[i] = source[size - 1 - i];
        }
    }
}

template <typename T>
concept Scalar = std::is_arithmetic_v<T>;

// Schreibt Werte hintereinander in einen Puffer, Zahlen in Little Endian, Texte mit vorangestellter
// Länge (u32). Das Format hat keine Feldnamen und keine Version: Es ist für Daten gedacht, die
// derselbe Build liest (Netzwerk, gekochte Assets), nicht für Dateien, die lange leben.
class BinaryWriter {
public:
    void write_bytes(std::span<const std::byte> bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    // Zahl mit size Bytes ab source in Little Endian anhängen.
    void write_scalar(const void* source, usize size) {
        const usize offset = buffer_.size();
        buffer_.resize(offset + size);
        copy_little_endian(buffer_.data() + offset, static_cast<const std::byte*>(source), size);
    }

    template <Scalar T>
    void write(T value) {
        if constexpr (std::is_same_v<T, bool>) {
            const u8 byte = value ? 1 : 0;
            write_scalar(&byte, 1);
        } else {
            write_scalar(&value, sizeof(T));
        }
    }

    void write_string(std::string_view text) {
        write(static_cast<u32>(text.size()));
        write_bytes(std::as_bytes(std::span{text.data(), text.size()}));
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return buffer_;
    }
    [[nodiscard]] std::vector<std::byte> take() noexcept {
        return std::move(buffer_);
    }

private:
    std::vector<std::byte> buffer_;
};

// Liest, was BinaryWriter geschrieben hat. Jede Funktion liefert false, wenn die Daten nicht
// reichen; dann bleibt die Position stehen. Längenangaben werden gegen die Restdaten geprüft, damit
// kaputte Daten keine riesigen Allokationen auslösen.
class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> data) noexcept : data_{data} {}

    [[nodiscard]] bool read_bytes(std::span<std::byte> target) noexcept {
        if (target.size() > remaining()) {
            return false;
        }
        std::memcpy(target.data(), data_.data() + position_, target.size());
        position_ += target.size();
        return true;
    }

    [[nodiscard]] bool read_scalar(void* target, usize size) noexcept {
        if (size > remaining()) {
            return false;
        }
        copy_little_endian(static_cast<std::byte*>(target), data_.data() + position_, size);
        position_ += size;
        return true;
    }

    template <Scalar T>
    [[nodiscard]] bool read(T& value) noexcept {
        if constexpr (std::is_same_v<T, bool>) {
            u8 byte = 0;
            if (!read_scalar(&byte, 1) || byte > 1) {
                return false;
            }
            value = byte == 1;
            return true;
        } else {
            return read_scalar(&value, sizeof(T));
        }
    }

    [[nodiscard]] bool read_string(std::string& text) {
        const usize start = position_;
        u32 size = 0;
        if (!read(size) || size > remaining()) {
            position_ = start;
            return false;
        }
        text.assign(reinterpret_cast<const char*>(data_.data() + position_), size);
        position_ += size;
        return true;
    }

    [[nodiscard]] usize remaining() const noexcept {
        return data_.size() - position_;
    }
    [[nodiscard]] usize position() const noexcept {
        return position_;
    }

private:
    std::span<const std::byte> data_;
    usize position_ = 0;
};

} // namespace ngine::serialize
