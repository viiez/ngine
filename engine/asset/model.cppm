module;

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module ngine.asset:model;

import ngine.core;
import ngine.math;
export import ngine.meshlet;
import :image;

export namespace ngine::asset {

// Ein Eckpunkt, so wie ihn der Shader über Vertex Pulling liest (48 Bytes, ohne Lücken). Die
// Tangente hat in w das Vorzeichen der Bitangente (wie in glTF).
struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec4 tangent;
    math::Vec2 uv;
};

// Teilnetz mit einem Material, immer als Dreiecksliste.
struct Primitive {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    // Index in Model::materials; ohne Material nimmt der Renderer sein Standardmaterial.
    std::optional<u32> material;
    // Die Datei hatte keine Tangenten; sie sind einfach erzeugt (generate_tangents()). Beim
    // Kochen ersetzt MikkTSpace sie und setzt das zurück (TD-21).
    bool generated_tangents = false;
    // Meshlets über die Dreiecke (R3.5, ADR 0043); indices ist dann nach Meshlets umsortiert. Leer
    // heißt: noch nicht aufgeteilt (ensure_meshlets()). Gekochte Modelle bringen sie aus
    // meshoptimizer mit.
    std::vector<meshlet::Meshlet> meshlets;
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
};

enum class Filter : u8 { Nearest, Linear };
enum class Wrap : u8 { Repeat, MirroredRepeat, ClampToEdge };

// Wie eine Textur gelesen wird. Ohne Angabe in der Datei: linear, Mipmaps, Wiederholen.
struct SamplerDesc {
    Filter mag = Filter::Linear;
    Filter min = Filter::Linear;
    Filter mip = Filter::Linear;
    bool mipmaps = true;
    Wrap wrap_u = Wrap::Repeat;
    Wrap wrap_v = Wrap::Repeat;

    friend bool operator==(const SamplerDesc&, const SamplerDesc&) = default;
};

struct Texture {
    u32 image = 0; // Index in Model::images
    SamplerDesc sampler;
};

enum class AlphaMode : u8 { Opaque, Mask, Blend };

// Material nach glTF Metallic-Roughness. Texturindizes zeigen in Model::textures. Grundfarbe und
// Emission sind sRGB, alle anderen Texturen linear. Metallic steht im Blaukanal, Roughness im
// Grünkanal der Metallic-Roughness-Textur.
struct Material {
    std::string name;
    math::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallic = 1.0f;
    f32 roughness = 1.0f;
    math::Vec3 emissive{0.0f, 0.0f, 0.0f};
    f32 normal_scale = 1.0f;
    f32 occlusion_strength = 1.0f;
    std::optional<u32> base_color_texture;
    std::optional<u32> metallic_roughness_texture;
    std::optional<u32> normal_texture;
    std::optional<u32> occlusion_texture;
    std::optional<u32> emissive_texture;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    f32 alpha_cutoff = 0.5f;
    bool double_sided = false;
};

// Knoten der Hierarchie mit Lage relativ zum Elternknoten.
struct Node {
    std::string name;
    math::Vec3 translation{0.0f, 0.0f, 0.0f};
    math::Quat rotation = math::quat_identity();
    math::Vec3 scale{1.0f, 1.0f, 1.0f};
    std::optional<u32> mesh;
    std::optional<u32> parent;
    std::vector<u32> children;
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Image> images;
    std::vector<Node> nodes;
    // Wurzelknoten der Szene, die gezeigt werden soll (Standardszene, sonst die erste, sonst
    // alle Knoten ohne Elternteil).
    std::vector<u32> roots;
};

// Teilt alle Teilnetze ohne Meshlets mit dem eigenen Verfahren (meshlet::build()) auf. Braucht
// keine GPU und läuft beim Laden im Hintergrund.
void ensure_meshlets(Model& model);

// Lädt eine glTF-2.0-Datei (.gltf mit eingebetteten oder externen Puffern, .glb) samt Bildern.
// Fehlerhafte Dateien liefern eine Meldung mit Pfad und Ursache, nie einen Absturz. Was nicht
// unterstützt wird (Punkte und Linien, Skins, Animationen, Morph Targets, weitere UV-Sätze), wird
// mit einer Warnung übergangen. dependencies bekommt die lokalen Dateien, aus denen externe
// Puffer und Bilder stammen (normalisiert), z. B. für Hot Reload.
[[nodiscard]] Result<Model> load_gltf(std::string_view path,
                                      std::vector<std::string>* dependencies = nullptr);

} // namespace ngine::asset
