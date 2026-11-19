module;

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module ngine.animation:rig;

import ngine.core;
import ngine.math;
import ngine.asset;

export namespace ngine::animation {

// Lage eines Knotens relativ zu seinem Elternknoten.
struct LocalTransform {
    math::Vec3 translation{0.0f, 0.0f, 0.0f};
    math::Quat rotation = math::quat_identity();
    math::Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Lokale Lagen aller Knoten eines Rigs, Index wie in asset::Model::nodes.
using Pose = std::vector<LocalTransform>;

// Zeilen einer affinen 3×4-Matrix, wie der Skinning-Shader sie liest (48 Bytes).
struct JointMatrix {
    math::Vec4 rows[3];
};

// Skelett und Clips eines Modells als Ressource (Laden über den Ressourcen-Manager mit der
// Kennung des Modells). Alle Knoten des Modells sind Teil des Skeletts; Gelenke eines Skins sind
// eine Auswahl davon.
struct Rig {
    // Elternknoten je Knoten (-1: Wurzel).
    std::vector<i32> parents;
    // Knoten in einer Reihenfolge, in der Eltern vor ihren Kindern stehen.
    std::vector<u32> order;
    std::vector<std::string> names;
    // Ruhelage aus der Datei.
    Pose rest;
    std::vector<asset::Skin> skins;
    std::vector<asset::Animation> clips;

    [[nodiscard]] usize node_count() const noexcept {
        return parents.size();
    }
    [[nodiscard]] std::optional<u32> find_clip(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<u32> find_node(std::string_view name) const noexcept;
};

[[nodiscard]] asset::Result<Rig> make_rig(const asset::Model& model);

// Pose eines Clips zur Zeit time (in Sekunden, auf [0, Dauer] geklemmt): Ruhelage, überschrieben
// von den Spuren des Clips. pose wird auf die Zahl der Knoten gebracht.
void sample(const Rig& rig, u32 clip, f32 time, Pose& pose);

// Mischt zwei Posen gleicher Größe: Verschiebung und Skalierung linear, Drehung über nlerp auf der
// kürzeren Seite. weight 0 ergibt a, 1 ergibt b. out darf a oder b sein.
void blend(const Pose& a, const Pose& b, f32 weight, Pose& out);

// Lagen aller Knoten im Raum des Modells.
void model_space(const Rig& rig, const Pose& pose, std::vector<math::Mat4>& out);

// Palette eines Skins: je Gelenk Lage im Modellraum mal inverse Bindematrix, als 3×4-Zeilen.
void palette(const Rig& rig, u32 skin, std::span<const math::Mat4> model,
             std::vector<JointMatrix>& out);

// Lader für Rig im Ressourcen-Manager: aus dem Cache gekochter Modelle, wenn aktuell, sonst aus
// der glTF-Datei. cache darf nullptr sein und muss den Manager sonst überleben.
void register_loaders(asset::ResourceManager& resources, const asset::AssetCache* cache = nullptr);

} // namespace ngine::animation
