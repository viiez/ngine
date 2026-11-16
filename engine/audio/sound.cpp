module;

#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.audio;

import ngine.core;
import ngine.asset;
import ngine.filesystem;
import ngine.serialize;

namespace ngine::audio {

Result<Sound> load_sound_file(std::string_view path) {
    auto bytes = filesystem::read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto info = probe_sound(*bytes);
    if (!info) {
        return std::unexpected(std::format("{}: {}", path, info.error()));
    }
    Sound sound;
    sound.info = *info;
    // Unbekannte Länge (frames 0) wird ganz dekodiert; solche Formate sind selten lang.
    if (info->frames > 0 && info->duration() > stream_threshold_seconds) {
        sound.stream_path = std::string{path};
        return sound;
    }
    auto data = decode_sound(*bytes);
    if (!data) {
        return std::unexpected(std::format("{}: {}", path, data.error()));
    }
    sound.info.frames = data->frames();
    sound.data = std::make_shared<const SoundData>(std::move(*data));
    return sound;
}

Result<SoundSet> parse_sound_set(std::string_view text) {
    auto root = json::parse(text);
    if (!root) {
        return std::unexpected(
            std::format("Zeile {}: {}", root.error().line, root.error().message));
    }
    const json::Value* format = root->find("format");
    if (format == nullptr || format->string() == nullptr || *format->string() != sound_set_format) {
        return std::unexpected(std::format("Kein Format \"{}\"", sound_set_format));
    }
    const json::Value* list = root->find("sounds");
    if (list == nullptr || list->array() == nullptr) {
        return std::unexpected(std::string{"Schlüssel \"sounds\" mit einer Liste fehlt"});
    }
    SoundSet set;
    for (const json::Value& entry : *list->array()) {
        const auto id = entry.to_u64();
        if (!id || *id == 0) {
            return std::unexpected(std::string{"\"sounds\" enthält keine Asset-Kennung"});
        }
        set.sounds.push_back(*id);
    }
    const auto number = [&](std::string_view key) {
        const json::Value* value = root->find(key);
        const auto f = value != nullptr ? value->to_f64() : std::nullopt;
        return f ? static_cast<f32>(*f) : 0.0f;
    };
    set.volume_variation = number("volume_variation");
    set.pitch_variation = number("pitch_variation");
    return set;
}

std::string write_sound_set(const SoundSet& set) {
    json::Array sounds;
    for (const u64 id : set.sounds) {
        sounds.emplace_back(id);
    }
    json::Value root;
    root["format"] = sound_set_format;
    root["version"] = sound_set_version;
    root["sounds"] = json::Value{std::move(sounds)};
    root["volume_variation"] = set.volume_variation;
    root["pitch_variation"] = set.pitch_variation;
    return json::write(root);
}

void register_loaders(asset::ResourceManager& resources) {
    resources.add_loader<Sound, Sound>(
        [](const asset::LoadContext& context) -> asset::Result<Sound> {
            auto sound = load_sound_file(context.path);
            if (!sound) {
                return std::unexpected(std::move(sound.error()));
            }
            return std::move(*sound);
        },
        [](Sound& sound) -> asset::Result<Sound> { return std::move(sound); });
    resources.add_loader<SoundSet, SoundSet>(
        [](const asset::LoadContext& context) -> asset::Result<SoundSet> {
            auto text = filesystem::read_text(context.path);
            if (!text) {
                return std::unexpected(std::move(text.error()));
            }
            auto set = parse_sound_set(*text);
            if (!set) {
                return std::unexpected(std::format("{}: {}", context.path, set.error()));
            }
            return std::move(*set);
        },
        [](SoundSet& set) -> asset::Result<SoundSet> { return std::move(set); });
}

} // namespace ngine::audio
