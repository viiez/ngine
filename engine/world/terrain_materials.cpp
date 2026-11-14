module;

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.world;

import ngine.core;
import ngine.filesystem;
import ngine.math;
import ngine.reflect;
import ngine.serialize;

namespace ngine::world {

namespace {

reflect::TypeRegistry material_types() {
    reflect::TypeRegistry types;
    register_terrain_material_types(types);
    return types;
}

} // namespace

void register_terrain_material_types(reflect::TypeRegistry& types) {
    if (types.find<TerrainMaterial>() != nullptr) {
        return;
    }
    if (types.find<math::Vec3>() == nullptr) {
        types.add<math::Vec3>("Vec3")
            .field("x", &math::Vec3::x)
            .field("y", &math::Vec3::y)
            .field("z", &math::Vec3::z);
    }
    types.add<TerrainMaterial>("TerrainMaterial")
        .field("name", &TerrainMaterial::name)
        .field("color", &TerrainMaterial::color)
        .hint(reflect::Hint::Color)
        .field("roughness", &TerrainMaterial::roughness)
        .range(0.0, 1.0, 0.01)
        .field("scale", &TerrainMaterial::scale)
        .range(0.1, 100.0, 0.1)
        .field("normal_scale", &TerrainMaterial::normal_scale)
        .range(0.0, 4.0, 0.01)
        .field("albedo", &TerrainMaterial::albedo)
        .hint(reflect::Hint::Asset)
        .field("normal", &TerrainMaterial::normal)
        .hint(reflect::Hint::Asset)
        .field("friction", &TerrainMaterial::friction)
        .range(0.0, 2.0, 0.01)
        .field("restitution", &TerrainMaterial::restitution)
        .range(0.0, 1.0, 0.01)
        .field("footsteps", &TerrainMaterial::footsteps);
}

TerrainMaterials standard_terrain_materials() {
    // Farben linear; mit Textur Faktoren darauf (die Texturen bringen die eigentliche Farbe).
    return {.materials = {
                {.name = "Luft", .friction = 0.0f},
                {.name = "Gras",
                 .color = {0.045f, 0.13f, 0.02f},
                 .roughness = 0.95f,
                 .scale = 3.0f,
                 .friction = 0.7f,
                 .footsteps = "gras"},
                {.name = "Fels",
                 .color = {0.16f, 0.15f, 0.135f},
                 .roughness = 0.85f,
                 .scale = 6.0f,
                 .friction = 0.8f,
                 .restitution = 0.1f,
                 .footsteps = "fels"},
                {.name = "Erde",
                 .color = {0.15f, 0.075f, 0.03f},
                 .roughness = 0.95f,
                 .scale = 3.0f,
                 .friction = 0.75f,
                 .footsteps = "erde"},
                {.name = "Sand",
                 .color = {0.45f, 0.38f, 0.24f},
                 .roughness = 0.9f,
                 .scale = 4.0f,
                 .friction = 0.55f,
                 .footsteps = "sand"},
            }};
}

Result<TerrainMaterials> parse_terrain_materials(std::string_view text) {
    auto root = json::parse(text);
    if (!root) {
        return std::unexpected(
            std::format("Zeile {}: {}", root.error().line, root.error().message));
    }
    const json::Value* format = root->find("format");
    if (format == nullptr || format->string() == nullptr ||
        *format->string() != terrain_materials_format) {
        return std::unexpected(std::format("Kein Format \"{}\"", terrain_materials_format));
    }
    const json::Value* list = root->find("materials");
    if (list == nullptr || list->array() == nullptr) {
        return std::unexpected(std::string{"Schlüssel \"materials\" mit einer Liste fehlt"});
    }
    if (list->array()->size() > max_terrain_materials) {
        return std::unexpected(std::format("Mehr als {} Materialien", max_terrain_materials));
    }
    const reflect::TypeRegistry types = material_types();
    TerrainMaterials table;
    for (const json::Value& entry : *list->array()) {
        TerrainMaterial material;
        if (auto read = serialize::from_json(entry, material, types); !read) {
            return std::unexpected(
                std::format("Material {}: {}", table.materials.size(), read.error()));
        }
        table.materials.push_back(std::move(material));
    }
    return table;
}

std::string write_terrain_materials(const TerrainMaterials& table) {
    const reflect::TypeRegistry types = material_types();
    json::Array materials;
    for (const TerrainMaterial& material : table.materials) {
        materials.push_back(serialize::to_json(material, types));
    }
    json::Value root;
    root["format"] = terrain_materials_format;
    root["version"] = terrain_materials_version;
    root["materials"] = json::Value{std::move(materials)};
    return json::write(root);
}

Result<TerrainMaterials> load_terrain_materials_file(std::string_view path) {
    auto text = filesystem::read_text(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto table = parse_terrain_materials(*text);
    if (!table) {
        return std::unexpected(std::format("{}: {}", path, table.error()));
    }
    return table;
}

Result<void> save_terrain_materials_file(const TerrainMaterials& table, std::string_view path) {
    return filesystem::write_text(path, write_terrain_materials(table));
}

} // namespace ngine::world
