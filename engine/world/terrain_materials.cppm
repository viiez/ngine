module;

#include <expected>
#include <string>
#include <string_view>
#include <vector>

// Materialtabelle des Terrains als Asset (VOX-07, R4.3, TD-37): eine Datei
// (terrain/standard.terrain-materials) für Renderer, Physik und Spiel. Die Reihenfolge in der Datei
// ist die Materialnummer der Voxel (voxel::Material); Eintrag 0 ist Luft und wird nie gezeichnet.
//
//   { "format": "ngine.terrain-materials", "version": 1,
//     "materials": [ { "name": "Luft", ... }, { "name": "Gras", "color": {...}, ... }, ... ] }
//
// Jedes Material steht über die Reflection in der Datei (wie die Aktionen der Eingabe, R4.1); die
// Liste liest und schreibt dieser Teil selbst, weil die Reflection keine Listen kennt.
export module ngine.world:terrain_materials;

import ngine.core;
import ngine.math;
import ngine.reflect;
import :prefab;

export namespace ngine::world {

inline constexpr std::string_view terrain_materials_format = "ngine.terrain-materials";
inline constexpr i64 terrain_materials_version = 1;

// Ein Material des Terrains: Aussehen für den Renderer, Oberfläche für die Physik und ein Name für
// Schritte (Geräusche in R4.6).
struct TerrainMaterial {
    std::string name;
    // Grundfarbe (linear); mit Textur ein Faktor darauf.
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    f32 roughness = 0.9f;
    // Meter pro Wiederholung der Texturen (triplanar).
    f32 scale = 4.0f;
    f32 normal_scale = 1.0f;
    // Asset-Kennungen von Grundfarbe (sRGB) und Normalen; 0 heißt keine.
    u64 albedo = 0;
    u64 normal = 0;
    // Reibung und Rückprall der Oberfläche (wie physics::RigidBody).
    f32 friction = 0.7f;
    f32 restitution = 0.0f;
    // Art der Schritte, z. B. „gras“ oder „fels“.
    std::string footsteps;

    friend bool operator==(const TerrainMaterial&, const TerrainMaterial&) = default;
};

struct TerrainMaterials {
    // Index ist die Materialnummer; höchstens 256 Einträge (8 Bit).
    std::vector<TerrainMaterial> materials;

    // Material zu einer Nummer, nullptr außerhalb der Tabelle.
    [[nodiscard]] const TerrainMaterial* find(u32 material) const noexcept {
        return material < materials.size() ? &materials[material] : nullptr;
    }

    friend bool operator==(const TerrainMaterials&, const TerrainMaterials&) = default;
};

inline constexpr usize max_terrain_materials = 256;

// Eingebaute Tabelle ohne Texturen: 0 Luft, 1 Gras, 2 Fels, 3 Erde, 4 Sand (wie voxel::grass usw.
// und der Generator sie setzt). Die Datei im Asset-Ordner muss bis auf die Texturen dazu passen.
[[nodiscard]] TerrainMaterials standard_terrain_materials();

[[nodiscard]] Result<TerrainMaterials> parse_terrain_materials(std::string_view text);
[[nodiscard]] std::string write_terrain_materials(const TerrainMaterials& table);
[[nodiscard]] Result<TerrainMaterials> load_terrain_materials_file(std::string_view path);
[[nodiscard]] Result<void> save_terrain_materials_file(const TerrainMaterials& table,
                                                       std::string_view path);

// Registriert TerrainMaterial (aus world::register_types()).
void register_terrain_material_types(reflect::TypeRegistry& types);

} // namespace ngine::world
