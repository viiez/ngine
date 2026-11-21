module;

#include <bit>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

module ngine.replication;

import ngine.core;
import ngine.input;

namespace ngine::replication {

namespace {

template <typename T>
void put(std::vector<std::byte>& out, T value) {
    for (usize i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
    }
}

template <typename T>
T get(std::span<const std::byte> data, usize offset) {
    T value = 0;
    for (usize i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(static_cast<T>(data[offset + i]) << (8 * i));
    }
    return value;
}

constexpr usize actions_header = 1 + 4 + 2;
constexpr usize action_size = 4 + 4 + 1;

} // namespace

std::optional<GameMessage> game_message(std::span<const std::byte> data) noexcept {
    if (data.empty()) {
        return std::nullopt;
    }
    const auto kind = static_cast<GameMessage>(data[0]);
    if (kind == GameMessage::Actions || kind == GameMessage::Possess || kind == GameMessage::View) {
        return kind;
    }
    return std::nullopt;
}

std::vector<std::byte> encode_actions(u32 map_hash, std::span<const input::ActionValue> values) {
    const usize count = values.size() > 0xFFFF ? 0xFFFF : values.size();
    std::vector<std::byte> out;
    out.reserve(actions_header + count * action_size);
    out.push_back(static_cast<std::byte>(GameMessage::Actions));
    put<u32>(out, map_hash);
    put<u16>(out, static_cast<u16>(count));
    for (usize i = 0; i < count; ++i) {
        const input::ActionValue& value = values[i];
        put<u32>(out, std::bit_cast<u32>(value.x));
        put<u32>(out, std::bit_cast<u32>(value.y));
        const u8 flags = static_cast<u8>((value.down ? 1u : 0u) | (value.pressed ? 2u : 0u) |
                                         (value.released ? 4u : 0u));
        out.push_back(static_cast<std::byte>(flags));
    }
    return out;
}

Result<std::vector<input::ActionValue>> decode_actions(std::span<const std::byte> data,
                                                       u32 map_hash) {
    if (data.size() < actions_header || game_message(data) != GameMessage::Actions) {
        return std::unexpected("Eingabe: Nachricht zu kurz oder falsche Art");
    }
    if (const u32 hash = get<u32>(data, 1); hash != map_hash) {
        return std::unexpected(
            std::format("Eingabe: Belegung passt nicht ({:08x} statt {:08x})", hash, map_hash));
    }
    const usize count = get<u16>(data, 5);
    if (data.size() != actions_header + count * action_size) {
        return std::unexpected("Eingabe: Länge passt nicht zur Zahl der Aktionen");
    }
    std::vector<input::ActionValue> values(count);
    for (usize i = 0; i < count; ++i) {
        const usize at = actions_header + i * action_size;
        const u8 flags = static_cast<u8>(data[at + 8]);
        values[i] = {.x = std::bit_cast<f32>(get<u32>(data, at)),
                     .y = std::bit_cast<f32>(get<u32>(data, at + 4)),
                     .down = (flags & 1u) != 0,
                     .pressed = (flags & 2u) != 0,
                     .released = (flags & 4u) != 0};
    }
    return values;
}

std::vector<std::byte> encode_possess(u64 entity) {
    std::vector<std::byte> out;
    out.reserve(9);
    out.push_back(static_cast<std::byte>(GameMessage::Possess));
    put<u64>(out, entity);
    return out;
}

Result<u64> decode_possess(std::span<const std::byte> data) {
    if (data.size() != 9 || game_message(data) != GameMessage::Possess) {
        return std::unexpected("Figur: Nachricht hat die falsche Länge oder Art");
    }
    return get<u64>(data, 1);
}

std::vector<std::byte> encode_view(f32 yaw, f32 pitch) {
    std::vector<std::byte> out;
    out.reserve(9);
    out.push_back(static_cast<std::byte>(GameMessage::View));
    put<u32>(out, std::bit_cast<u32>(yaw));
    put<u32>(out, std::bit_cast<u32>(pitch));
    return out;
}

Result<ViewAngles> decode_view(std::span<const std::byte> data) {
    if (data.size() != 9 || game_message(data) != GameMessage::View) {
        return std::unexpected("Blick: Nachricht hat die falsche Länge oder Art");
    }
    const ViewAngles view{.yaw = std::bit_cast<f32>(get<u32>(data, 1)),
                          .pitch = std::bit_cast<f32>(get<u32>(data, 5))};
    if (!std::isfinite(view.yaw) || !std::isfinite(view.pitch)) {
        return std::unexpected("Blick: keine endlichen Winkel");
    }
    return view;
}

} // namespace ngine::replication
