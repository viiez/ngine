module;

#include <array>
#include <span>
#include <vector>

// GPU-getriebenes Zeichnen (R3.5, ADR 0043): Daten, die die GPU zum Prüfen der Meshlets liest
// (driven.slang), und die Rechnungen dafür, die keine GPU brauchen und deshalb hier testbar sind.
export module ngine.render:culling;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.meshlet;

export namespace ngine::render {

// Meshlets, die eine Arbeitsgruppe von Task- bzw. Compute-Shader prüft.
inline constexpr u32 group_meshlets = 32;
// Größte Zahl von Stufen des Hi-Z (Stufe 0 bis 32768 Pixel).
inline constexpr u32 max_hiz_levels = 16;

// Ein Meshlet, wie driven.slang es liest.
struct GpuMeshlet {
    // xyz: Mittelpunkt der Hüllkugel in Koordinaten der Eckpunkte, w: Radius.
    math::Vec4 sphere;
    // xyz: Achse des Normalenkegels, w: Grenze (1: kein Test).
    math::Vec4 cone;
    // Wort ab dem Anfang der Meshlet-Daten des Netzes, an dem die Eckpunkte des Meshlets beginnen
    // (Indizes ab der Adresse der Eckpunkte), bzw. seine Dreiecke (drei lokale Indizes je Wort).
    u32 vertex_offset = 0;
    u32 triangle_offset = 0;
    // Erstes Dreieck im umsortierten Indexpuffer des Teilnetzes.
    u32 first_triangle = 0;
    // Bits 0–7: Eckpunkte, 8–15: Dreiecke.
    u32 counts = 0;
};
static_assert(sizeof(GpuMeshlet) == 48);

// Instanz ohne Kegeltest (doppelseitiges Material).
inline constexpr u32 instance_double_sided = 1;

// Ein Netz, das der Renderer in diesem Frame zeichnet: ein Teilnetz eines MeshRenderers oder ein
// Chunk eines Terrains, wie driven.slang es liest.
struct GpuInstance {
    // Zeilen der affinen Matrix von Objekt- in kamerarelative Weltkoordinaten.
    math::Vec4 model[3];
    // Hüllkugel (kamerarelativ, für durchsichtige Netze zum Sortieren).
    math::Vec4 sphere;
    // Nur Terrain: Ecke des Chunks relativ zum Terrain in Metern (xyz), Meter pro Voxel (w).
    math::Vec4 offset;
    u64 vertices = 0;
    // Anfang der Meshlet-Daten des Netzes (pack_meshlets()), zweimal: als Meshlets und als Wörter.
    u64 meshlets = 0;
    u64 words = 0;
    u32 first_meshlet = 0;
    u32 meshlet_count = 0;
    // Material im MaterialStore (Meshes).
    u32 material = 0;
    // Untere 32 Bit der Entity für den ID-Puffer.
    u32 id = 0;
    // Nur Terrain: Materialnummern der acht Palettenplätze, vier je Wort.
    u32 palette[2] = {};
    // Erster Index des Teilnetzes im Indexpuffer seines Blocks.
    u32 first_index = 0;
    u32 flags = 0;
    u32 reserved[2] = {};
};
static_assert(sizeof(GpuInstance) == 144);

// Schalter einer Sicht (GpuView::params[0]).
inline constexpr u32 view_cone = 1;
inline constexpr u32 view_previous_hiz = 2;
inline constexpr u32 view_phase_two = 4;

// Eine Sicht, gegen die die GPU Meshlets prüft: Kamera, Kaskade der Schatten oder ID-Puffer.
struct GpuView {
    // Ebenen des Stumpfs (frustum_planes()).
    math::Vec4 planes[6];
    // Matrizen (Zeilen) dieses und des letzten Frames, Versatz vom Ursprung dieses Frames zu dem
    // des letzten (xyz).
    math::Vec4 view_projection[4];
    math::Vec4 previous_view_projection[4];
    math::Vec4 previous_offset;
    // Bindlose Indizes der Stufen des Hi-Z dieses und des letzten Frames.
    u32 hiz_current[max_hiz_levels] = {};
    u32 hiz_previous[max_hiz_levels] = {};
    // Schalter (view_*), Zahl der Stufen, Größe der Stufe 0.
    u32 params[4] = {};
};
static_assert(sizeof(GpuView) == 384);

// Die sechs Ebenen des Stumpfs einer Matrix in Clip-Koordinaten mit Tiefe in [0, 1] (Vulkan):
// links, rechts, unten, oben, z ≥ 0, z ≤ w. Normalen zeigen nach innen und sind normiert; ein Punkt
// p liegt innen, wenn dot(xyz, p) + w ≥ 0. Eine Ebene im Unendlichen (umgekehrte Tiefe mit
// unendlich fernem Horizont) wird zu (0, 0, 0, 1) und lässt alles durch.
[[nodiscard]] std::array<math::Vec4, 6> frustum_planes(const math::Mat4& view_projection);

// Die Meshlets eines Teilnetzes und seine umsortierten Indizes (ab der Adresse der Eckpunkte des
// ganzen Netzes).
struct MeshletPart {
    std::span<const meshlet::Meshlet> meshlets;
    std::span<const u32> indices;
};

// Meshlet-Daten eines Netzes für die GPU: erst ein GpuMeshlet je Meshlet aller Teile (in der
// Reihenfolge der Teile), dann je Teil die Eckpunkte und die Dreiecke seiner Meshlets als Wörter
// (meshlet::local_data()).
[[nodiscard]] std::vector<u32> pack_meshlets(std::span<const MeshletPart> parts);

// Größe der Stufe 0 des Hi-Z: in jeder Achse die größte Zweierpotenz, die in die Tiefe passt.
[[nodiscard]] rhi::Extent2D hiz_extent(rhi::Extent2D depth) noexcept;
// Stufen bis 1 × 1, höchstens max_hiz_levels.
[[nodiscard]] u32 hiz_level_count(rhi::Extent2D hiz) noexcept;

} // namespace ngine::render
