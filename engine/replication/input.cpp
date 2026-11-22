module;

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <deque>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

module ngine.replication;

import ngine.core;
import ngine.input;
import ngine.math;

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

void put_f32(std::vector<std::byte>& out, f32 value) {
    put<u32>(out, std::bit_cast<u32>(value));
}

f32 get_f32(std::span<const std::byte> data, usize offset) {
    return std::bit_cast<f32>(get<u32>(data, offset));
}

// Art, Prüfsumme, Zahl der Aktionen, Zahl der Befehle.
constexpr usize commands_header = 1 + 4 + 2 + 1;
// Nummer, yaw, pitch.
constexpr usize command_header = 4 + 4 + 4;
constexpr u8 flag_down = 1;
constexpr u8 flag_pressed = 2;
constexpr u8 flag_released = 4;
constexpr u8 flag_value = 8;

constexpr usize owner_size = 1 + 4 + 1 + 3 * 8 + 3 * 4 + 1 + 1;

} // namespace

std::optional<GameMessage> game_message(std::span<const std::byte> data) noexcept {
    if (data.empty()) {
        return std::nullopt;
    }
    const auto kind = static_cast<GameMessage>(data[0]);
    if (kind == GameMessage::Commands || kind == GameMessage::Possess ||
        kind == GameMessage::Owner) {
        return kind;
    }
    return std::nullopt;
}

std::vector<std::byte> encode_commands(u32 map_hash, std::span<const Command> commands) {
    const usize count = std::min(commands.size(), max_commands_per_message);
    const std::span<const Command> sent = commands.last(count);
    const usize actions = sent.empty() ? 0 : std::min<usize>(sent.front().actions.size(), 0xFFFF);
    std::vector<std::byte> out;
    out.reserve(commands_header + count * (command_header + actions * 9));
    out.push_back(static_cast<std::byte>(GameMessage::Commands));
    put<u32>(out, map_hash);
    put<u16>(out, static_cast<u16>(actions));
    out.push_back(static_cast<std::byte>(count));
    for (const Command& command : sent) {
        put<u32>(out, command.number);
        put_f32(out, command.yaw);
        put_f32(out, command.pitch);
        for (usize i = 0; i < actions; ++i) {
            const input::ActionValue value =
                i < command.actions.size() ? command.actions[i] : input::ActionValue{};
            const bool has_value = value.x != 0.0f || value.y != 0.0f;
            const u8 flags = static_cast<u8>(
                (value.down ? flag_down : 0) | (value.pressed ? flag_pressed : 0) |
                (value.released ? flag_released : 0) | (has_value ? flag_value : 0));
            out.push_back(static_cast<std::byte>(flags));
            if (has_value) {
                put_f32(out, value.x);
                put_f32(out, value.y);
            }
        }
    }
    return out;
}

Result<std::vector<Command>> decode_commands(std::span<const std::byte> data, u32 map_hash) {
    if (data.size() < commands_header || game_message(data) != GameMessage::Commands) {
        return std::unexpected("Befehle: Nachricht zu kurz oder falsche Art");
    }
    if (const u32 hash = get<u32>(data, 1); hash != map_hash) {
        return std::unexpected(
            std::format("Befehle: Belegung passt nicht ({:08x} statt {:08x})", hash, map_hash));
    }
    const usize actions = get<u16>(data, 5);
    const usize count = static_cast<u8>(data[7]);
    if (count == 0 || count > max_commands_per_message) {
        return std::unexpected(std::format("Befehle: {} Befehle in einer Nachricht", count));
    }
    std::vector<Command> commands(count);
    usize at = commands_header;
    const auto finite = [](f32 value) { return std::isfinite(value); };
    for (Command& command : commands) {
        if (data.size() < at + command_header + actions) {
            return std::unexpected("Befehle: Nachricht ist unvollständig");
        }
        command.number = get<u32>(data, at);
        command.yaw = get_f32(data, at + 4);
        command.pitch = get_f32(data, at + 8);
        at += command_header;
        if (!finite(command.yaw) || !finite(command.pitch)) {
            return std::unexpected("Befehle: Blick ohne endliche Winkel");
        }
        command.actions.resize(actions);
        for (input::ActionValue& value : command.actions) {
            if (at >= data.size()) {
                return std::unexpected("Befehle: Nachricht ist unvollständig");
            }
            const u8 flags = static_cast<u8>(data[at++]);
            if ((flags & ~(flag_down | flag_pressed | flag_released | flag_value)) != 0) {
                return std::unexpected("Befehle: unbekannte Flanken");
            }
            value.down = (flags & flag_down) != 0;
            value.pressed = (flags & flag_pressed) != 0;
            value.released = (flags & flag_released) != 0;
            if ((flags & flag_value) != 0) {
                if (data.size() < at + 8) {
                    return std::unexpected("Befehle: Nachricht ist unvollständig");
                }
                value.x = get_f32(data, at);
                value.y = get_f32(data, at + 4);
                at += 8;
                if (!finite(value.x) || !finite(value.y)) {
                    return std::unexpected("Befehle: Wert einer Aktion ist nicht endlich");
                }
            }
        }
    }
    if (at != data.size()) {
        return std::unexpected("Befehle: überzählige Bytes");
    }
    for (usize i = 1; i < commands.size(); ++i) {
        if (commands[i].number <= commands[i - 1].number) {
            return std::unexpected("Befehle: Nummern nicht aufsteigend");
        }
    }
    return commands;
}

u32 limit_command(Command& command, const input::ActionMap& map, f32 max_pitch) {
    u32 changed = 0;
    if (command.pitch > max_pitch || command.pitch < -max_pitch) {
        command.pitch = std::clamp(command.pitch, -max_pitch, max_pitch);
        ++changed;
    }
    const auto& actions = map.actions();
    for (usize i = 0; i < command.actions.size() && i < actions.size(); ++i) {
        const input::Action& action = actions[i];
        input::ActionValue& value = command.actions[i];
        switch (action.kind) {
        case input::ActionKind::Button:
            if (value.x < 0.0f || value.x > 1.0f || value.y != 0.0f) {
                value.x = std::clamp(value.x, 0.0f, 1.0f);
                value.y = 0.0f;
                ++changed;
            }
            break;
        case input::ActionKind::Axis:
            if (value.y != 0.0f) {
                value.y = 0.0f;
                ++changed;
            }
            if (action.limit > 0.0f && std::abs(value.x) > action.limit) {
                value.x = std::clamp(value.x, -action.limit, action.limit);
                ++changed;
            }
            break;
        case input::ActionKind::Axis2:
            if (action.limit > 0.0f) {
                const f32 length = std::sqrt(value.x * value.x + value.y * value.y);
                // Etwas Spielraum für Rundung, damit WASD diagonal nicht als Fehler zählt.
                if (length > action.limit * 1.0001f) {
                    const f32 scale = action.limit / length;
                    value.x *= scale;
                    value.y *= scale;
                    ++changed;
                }
            }
            break;
        }
    }
    return changed;
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

std::vector<std::byte> encode_owner(const OwnerState& state) {
    std::vector<std::byte> out;
    out.reserve(owner_size);
    out.push_back(static_cast<std::byte>(GameMessage::Owner));
    put<u32>(out, state.command);
    out.push_back(static_cast<std::byte>(state.queued));
    put<u64>(out, std::bit_cast<u64>(state.position.x));
    put<u64>(out, std::bit_cast<u64>(state.position.y));
    put<u64>(out, std::bit_cast<u64>(state.position.z));
    put_f32(out, state.velocity.x);
    put_f32(out, state.velocity.y);
    put_f32(out, state.velocity.z);
    out.push_back(static_cast<std::byte>(state.ground));
    out.push_back(static_cast<std::byte>(state.crouched ? 1 : 0));
    return out;
}

Result<OwnerState> decode_owner(std::span<const std::byte> data) {
    if (data.size() != owner_size || game_message(data) != GameMessage::Owner) {
        return std::unexpected("Stand der Figur: Nachricht hat die falsche Länge oder Art");
    }
    OwnerState state;
    state.command = get<u32>(data, 1);
    state.queued = static_cast<u8>(data[5]);
    state.position = {std::bit_cast<f64>(get<u64>(data, 6)), std::bit_cast<f64>(get<u64>(data, 14)),
                      std::bit_cast<f64>(get<u64>(data, 22))};
    state.velocity = {get_f32(data, 30), get_f32(data, 34), get_f32(data, 38)};
    state.ground = static_cast<u8>(data[42]);
    state.crouched = data[43] != std::byte{0};
    if (!std::isfinite(state.position.x) || !std::isfinite(state.position.y) ||
        !std::isfinite(state.position.z) || !std::isfinite(state.velocity.x) ||
        !std::isfinite(state.velocity.y) || !std::isfinite(state.velocity.z)) {
        return std::unexpected("Stand der Figur: keine endlichen Werte");
    }
    return state;
}

CommandQueue::CommandQueue(usize target, usize capacity)
    : target_{std::max<usize>(target, 1)}, capacity_{std::max(capacity, target_)} {}

void CommandQueue::push(Command command) {
    ++stats_.received;
    if (started_ && command.number < next_) {
        ++stats_.dropped;
        return;
    }
    const auto at = std::ranges::lower_bound(queue_, command.number, {}, &Command::number);
    if (at != queue_.end() && at->number == command.number) {
        ++stats_.dropped;
        return;
    }
    queue_.insert(at, std::move(command));
    while (queue_.size() > capacity_) {
        if (started_) {
            next_ = std::max(next_, queue_.front().number + 1);
        }
        queue_.pop_front();
        ++stats_.overflow;
    }
}

std::optional<Command> CommandQueue::next() {
    if (!started_) {
        if (queue_.size() < target_) {
            return std::nullopt;
        }
        started_ = true;
        next_ = queue_.front().number;
    }
    while (!queue_.empty() && queue_.front().number < next_) {
        queue_.pop_front();
        ++stats_.dropped;
    }
    Command command;
    if (!queue_.empty() && queue_.front().number == next_) {
        command = std::move(queue_.front());
        queue_.pop_front();
    } else if (last_) {
        // Fehlt: der letzte noch einmal, Flanken zählen nur einmal.
        command = *last_;
        command.number = next_;
        for (input::ActionValue& value : command.actions) {
            value.pressed = false;
            value.released = false;
        }
        ++stats_.repeated;
    } else {
        // Der erste fehlt: mit dem frühesten weitermachen, der da ist.
        command = std::move(queue_.front());
        queue_.pop_front();
    }
    next_ = command.number + 1;
    ++stats_.processed;
    last_ = command;
    return command;
}

} // namespace ngine::replication
