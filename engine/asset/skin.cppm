module;

#include <span>
#include <vector>

export module ngine.asset:skin;

import ngine.core;
import ngine.math;
import :model;

// Hilfen für verformte Netze (R4.6, ANIM-01), ohne GPU: Lagen der Knoten aus einem Clip, Paletten
// und Hüllen der Meshlets. Das schnelle Abtasten für jedes Bild steckt in ngine.animation; hier
// liegt, was Import, Kochen und Tests brauchen.
export namespace ngine::asset {

// Wert einer Spur zur Zeit time (vor dem ersten Schlüssel der erste, nach dem letzten der letzte).
// out bekommt drei bzw. vier Zahlen; Drehungen werden über nlerp auf der kürzeren Seite gemischt.
void sample_channel(const AnimationChannel& channel, f32 time, f32* out) noexcept;

// Lagen aller Knoten im Modellraum: Ruhelage aus Node, überschrieben von den Spuren des Clips zur
// Zeit time (ohne Clip die Ruhelage).
[[nodiscard]] std::vector<math::Mat4> node_matrices(const Model& model, const Animation* animation,
                                                    f32 time);

// Palette eines Skins: je Gelenk Lage im Modellraum mal inverse Bindematrix.
[[nodiscard]] std::vector<math::Mat4> skin_palette(const Model& model, u32 skin,
                                                   std::span<const math::Mat4> nodes);

// Position eines Eckpunkts nach linearem Skinning mit der Palette.
[[nodiscard]] math::Vec3 skin_position(const SkinVertex& skin, math::Vec3 position,
                                       std::span<const math::Mat4> palette) noexcept;

// Vergrößert die Hüllkugeln der Meshlets verformter Netze, sodass sie das Meshlet in der Ruhelage
// und in jeder Pose aller Clips des Modells (abgetastet mit 30 Hz) umschließen, und schaltet den
// Test am Normalenkegel ab. Braucht Meshlets (ensure_meshlets() bzw. das Kochen).
void fit_skinned_meshlets(Model& model);

} // namespace ngine::asset
