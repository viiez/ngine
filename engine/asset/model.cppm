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

// Einfluss der Gelenke auf einen Eckpunkt (R4.6, ANIM-01): bis zu vier Gelenke (Index in
// Skin::joints) mit Gewichten als normierte u16 (65535 = 1), Summe 65535. 16 Bytes; liegt als
// zweiter Strom neben Vertex, damit starre Netze bei 48 Bytes bleiben.
struct SkinVertex {
    u16 joints[4] = {};
    u16 weights[4] = {};
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
    // Gelenke und Gewichte je Eckpunkt (R4.6), gleich viele wie vertices; leer bei starren Netzen.
    std::vector<SkinVertex> skin;
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
    // Skin, mit dem das Netz verformt wird (Index in Model::skins), aus dem ersten Knoten, der das
    // Netz mit einem Skin zeigt. Ohne Skin ist das Netz starr.
    std::optional<u32> skin;
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
    std::optional<u32> skin;
    std::optional<u32> parent;
    std::vector<u32> children;
};

// Skin aus glTF (R4.6): Gelenke als Knotenindizes und je Gelenk die inverse Bindematrix (vom
// Modellraum des Netzes in den Raum des Gelenks in der Bindepose).
struct Skin {
    std::string name;
    std::vector<u32> joints;
    std::vector<math::Mat4> inverse_bind;
};

enum class AnimationPath : u8 { Translation, Rotation, Scale };
// CUBICSPLINE tastet der Import auf 30 Hz linear ab.
enum class Interpolation : u8 { Step, Linear };

// Eine Spur eines Clips: Zeiten in Sekunden (aufsteigend) und je Zeit ein Wert (drei Zahlen für
// Verschiebung und Skalierung, vier für Drehung als x, y, z, w).
struct AnimationChannel {
    u32 node = 0;
    AnimationPath path = AnimationPath::Translation;
    Interpolation interpolation = Interpolation::Linear;
    std::vector<f32> times;
    std::vector<f32> values;
};

// Clip aus glTF; Spuren zeigen auf Knoten. Dauer ist die letzte Zeit aller Spuren.
struct Animation {
    std::string name;
    f32 duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Image> images;
    std::vector<Node> nodes;
    std::vector<Skin> skins;
    std::vector<Animation> animations;
    // Wurzelknoten der Szene, die gezeigt werden soll (Standardszene, sonst die erste, sonst
    // alle Knoten ohne Elternteil).
    std::vector<u32> roots;
};

// Teilt alle Teilnetze ohne Meshlets mit dem eigenen Verfahren (meshlet::build()) auf. Braucht
// keine GPU und läuft beim Laden im Hintergrund.
void ensure_meshlets(Model& model);

// Größte Zahl Gelenke in einem Skin (Paletten und Indizes in SkinVertex sind darauf ausgelegt).
inline constexpr u32 max_joints = 256;

// Lädt eine glTF-2.0-Datei (.gltf mit eingebetteten oder externen Puffern, .glb) samt Bildern.
// Fehlerhafte Dateien liefern eine Meldung mit Pfad und Ursache, nie einen Absturz. Was nicht
// unterstützt wird (Punkte und Linien, Morph Targets, weitere UV-Sätze, mehr als vier Einflüsse je
// Eckpunkt), wird mit einer Warnung übergangen bzw. gekürzt. dependencies bekommt die lokalen Dateien, aus denen externe
// Puffer und Bilder stammen (normalisiert), z. B. für Hot Reload.
[[nodiscard]] Result<Model> load_gltf(std::string_view path,
                                      std::vector<std::string>* dependencies = nullptr);

} // namespace ngine::asset
