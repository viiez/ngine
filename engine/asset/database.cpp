module;

#include <algorithm>
#include <expected>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module ngine.asset;

import ngine.core;
import ngine.filesystem;
import ngine.serialize;

namespace ngine::asset {

AssetId random_id() {
    // random_device liefert unter Linux und Windows echten Zufall; mt19937_64 streckt ihn.
    static thread_local std::mt19937_64 engine{[] {
        std::random_device device;
        return (u64{device()} << 32U) ^ device();
    }()};
    AssetId id = 0;
    while (id == 0) {
        id = engine();
    }
    return id;
}

AssetType type_of(std::string_view path) {
    const std::string extension = filesystem::extension(path);
    if (extension == ".gltf" || extension == ".glb") {
        return AssetType::Model;
    }
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" ||
        extension == ".bmp") {
        return AssetType::Texture;
    }
    if (extension == ".slang") {
        return AssetType::Shader;
    }
    if (extension == ".json" || extension == ".nscene") {
        return AssetType::Scene;
    }
    if (extension == ".prefab") {
        return AssetType::Prefab;
    }
    if (extension == ".input") {
        return AssetType::Input;
    }
    return AssetType::Unknown;
}

std::string_view to_string(AssetType type) {
    switch (type) {
    case AssetType::Model: return "Modell";
    case AssetType::Texture: return "Textur";
    case AssetType::Shader: return "Shader";
    case AssetType::Scene: return "Szene";
    case AssetType::Prefab: return "Prefab";
    case AssetType::Input: return "Eingabe";
    case AssetType::Unknown: break;
    }
    return "Unbekannt";
}

std::string write_meta(AssetId id) {
    json::Value meta;
    meta["format"] = meta_format;
    meta["version"] = meta_version;
    meta["id"] = id;
    return json::write(meta);
}

Result<AssetId> parse_meta(std::string_view text) {
    auto parsed = json::parse(text);
    if (!parsed) {
        return std::unexpected(std::format("kein gültiges JSON (Zeile {}): {}", parsed.error().line,
                                           parsed.error().message));
    }
    const json::Value* format = parsed->find("format");
    if (format == nullptr || format->string() == nullptr || *format->string() != meta_format) {
        return std::unexpected(std::string{"\"format\" fehlt oder passt nicht"});
    }
    const json::Value* version = parsed->find("version");
    if (version == nullptr || version->to_i64() != meta_version) {
        return std::unexpected(
            std::format("Version wird nicht unterstützt (erwartet {})", meta_version));
    }
    const json::Value* id = parsed->find("id");
    const auto value = id != nullptr ? id->to_u64() : std::nullopt;
    if (!value || *value == 0) {
        return std::unexpected(std::string{"\"id\" fehlt oder ist 0"});
    }
    return *value;
}

Result<AssetDatabase> AssetDatabase::open(std::string_view root) {
    if (!filesystem::is_directory(root)) {
        return std::unexpected(std::format("Asset-Ordner '{}' gibt es nicht", root));
    }
    AssetDatabase database;
    database.root_ = filesystem::normalize(root);
    if (auto result = database.refresh(); !result) {
        return std::unexpected(std::move(result.error()));
    }
    return database;
}

Result<DatabaseChanges> AssetDatabase::refresh() {
    auto entries = filesystem::list_directory(root_, true);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }
    std::unordered_set<std::string> files;
    for (const filesystem::FileEntry& entry : *entries) {
        if (!entry.directory) {
            files.insert(entry.path);
        }
    }

    DatabaseChanges changes;
    std::unordered_set<AssetId> before;
    for (const AssetInfo& info : assets_) {
        before.insert(info.id);
    }
    std::vector<AssetInfo> assets;
    std::unordered_set<AssetId> taken;
    for (const filesystem::FileEntry& entry : *entries) {
        const AssetType type = type_of(entry.path);
        if (entry.directory || type == AssetType::Unknown) {
            continue;
        }
        const std::string file = root_ + "/" + entry.path;
        const std::string meta_file = file + std::string{meta_extension};
        AssetId id = 0;
        if (files.contains(entry.path + std::string{meta_extension})) {
            if (auto text = filesystem::read_text(meta_file)) {
                if (auto parsed = parse_meta(*text)) {
                    id = *parsed;
                } else {
                    log::warn("Asset {}: .meta-Datei ungültig ({}), bekommt eine neue Kennung",
                              entry.path, parsed.error());
                }
            }
        }
        if (id != 0 && taken.contains(id)) {
            log::warn("Asset {}: Kennung {} ist doppelt (kopierte .meta-Datei?), bekommt eine "
                      "neue",
                      entry.path, id);
            id = 0;
        }
        if (id == 0) {
            id = random_id();
            if (auto written = filesystem::write_text(meta_file, write_meta(id)); !written) {
                log::error("Asset {}: {}", entry.path, written.error());
            } else {
                ++changes.created_meta;
                log::info("Asset {}: neue Kennung {}", entry.path, id);
            }
        }
        taken.insert(id);
        if (!before.contains(id)) {
            ++changes.added;
        }
        assets.push_back({.id = id, .type = type, .path = entry.path, .file = file});
    }
    for (const AssetId id : before) {
        if (!taken.contains(id)) {
            ++changes.removed;
        }
    }
    assets_ = std::move(assets);
    rebuild_index();
    return changes;
}

void AssetDatabase::rebuild_index() {
    by_id_.clear();
    by_file_.clear();
    for (usize i = 0; i < assets_.size(); ++i) {
        by_id_.emplace(assets_[i].id, i);
        by_file_.emplace(assets_[i].file, i);
    }
}

const AssetInfo* AssetDatabase::find(AssetId id) const {
    const auto found = by_id_.find(id);
    return found != by_id_.end() ? &assets_[found->second] : nullptr;
}

const AssetInfo* AssetDatabase::find_path(std::string_view path) const {
    if (root_.empty()) {
        return nullptr;
    }
    // Relative Pfade gelten relativ zur Wurzel, nicht zum Arbeitsordner.
    const bool absolute = path.starts_with('/') || (path.size() > 1 && path[1] == ':');
    const std::string file =
        filesystem::normalize(absolute ? std::string{path} : root_ + "/" + std::string{path});
    const auto found = by_file_.find(file);
    return found != by_file_.end() ? &assets_[found->second] : nullptr;
}

} // namespace ngine::asset
