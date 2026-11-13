module;

#include <span>

// Eingebaute Shader als SPIR-V. Die Daten erzeugt CMake beim Bauen aus den .slang-Dateien.
export module ngine.shaders;

import ngine.core;
import ngine.rhi;

namespace ngine::shaders::detail {

constexpr u32 triangle_vertex[] = {
#include "triangle_vertex.spv.inc"
};

constexpr u32 triangle_fragment[] = {
#include "triangle_fragment.spv.inc"
};

constexpr u32 voxel_vertex[] = {
#include "voxel_vertex.spv.inc"
};

constexpr u32 voxel_fragment[] = {
#include "voxel_fragment.spv.inc"
};

constexpr u32 fullscreen_vertex[] = {
#include "fullscreen_vertex.spv.inc"
};

constexpr u32 fullscreen_fragment[] = {
#include "fullscreen_fragment.spv.inc"
};

constexpr u32 pbr_vertex[] = {
#include "pbr_vertex.spv.inc"
};

constexpr u32 pbr_fragment[] = {
#include "pbr_fragment.spv.inc"
};

constexpr u32 shadow_vertex[] = {
#include "shadow_vertex.spv.inc"
};

constexpr u32 shadow_fragment[] = {
#include "shadow_fragment.spv.inc"
};

constexpr u32 debug_vertex[] = {
#include "debug_vertex.spv.inc"
};

constexpr u32 debug_fragment[] = {
#include "debug_fragment.spv.inc"
};

constexpr u32 id_vertex[] = {
#include "id_vertex.spv.inc"
};

constexpr u32 id_fragment[] = {
#include "id_fragment.spv.inc"
};

constexpr u32 terrain_vertex[] = {
#include "terrain_vertex.spv.inc"
};

constexpr u32 terrain_fragment[] = {
#include "terrain_fragment.spv.inc"
};

constexpr u32 terrain_shadow_vertex[] = {
#include "terrain_shadow_vertex.spv.inc"
};

constexpr u32 terrain_id_vertex[] = {
#include "terrain_id_vertex.spv.inc"
};

constexpr u32 terrain_id_fragment[] = {
#include "terrain_id_fragment.spv.inc"
};

constexpr u32 tonemap_fragment[] = {
#include "tonemap_fragment.spv.inc"
};

constexpr u32 ui_vertex[] = {
#include "ui_vertex.spv.inc"
};

constexpr u32 ui_fragment[] = {
#include "ui_fragment.spv.inc"
};

constexpr u32 gradient_compute[] = {
#include "gradient_compute.spv.inc"
};

constexpr u32 pbr_mesh[] = {
#include "pbr_mesh.spv.inc"
};

constexpr u32 shadow_mesh[] = {
#include "shadow_mesh.spv.inc"
};

constexpr u32 id_mesh[] = {
#include "id_mesh.spv.inc"
};

constexpr u32 terrain_mesh[] = {
#include "terrain_mesh.spv.inc"
};

constexpr u32 terrain_shadow_mesh[] = {
#include "terrain_shadow_mesh.spv.inc"
};

constexpr u32 terrain_id_mesh[] = {
#include "terrain_id_mesh.spv.inc"
};

constexpr u32 cull_task[] = {
#include "cull_task.spv.inc"
};

constexpr u32 cull_compute[] = {
#include "cull_compute.spv.inc"
};

constexpr u32 hiz_compute[] = {
#include "hiz_compute.spv.inc"
};

} // namespace ngine::shaders::detail

export namespace ngine::shaders {

rhi::ShaderDesc triangle_vertex() {
    return {rhi::ShaderStage::Vertex, detail::triangle_vertex, "vertex_main"};
}

rhi::ShaderDesc triangle_fragment() {
    return {rhi::ShaderStage::Fragment, detail::triangle_fragment, "fragment_main"};
}

// Voxel-Terrain (Durchstich D1). Erwartet Push Constants wie voxel.slang: Matrix in Zeilen
// (64 Bytes), Licht (16), Kamera mit Nebel (16) und die Adresse der Eckpunkte (8).
rhi::ShaderDesc voxel_vertex() {
    return {rhi::ShaderStage::Vertex, detail::voxel_vertex, "vertex_main"};
}

rhi::ShaderDesc voxel_fragment() {
    return {rhi::ShaderStage::Fragment, detail::voxel_fragment, "fragment_main"};
}

// Zeichnet eine Textur über das ganze Ziel (drei Eckpunkte, ohne Puffer). Push Constants: Index
// der Textur und des Samplers im bindlosen Satz (je u32).
rhi::ShaderDesc fullscreen_vertex() {
    return {rhi::ShaderStage::Vertex, detail::fullscreen_vertex, "vertex_main"};
}

rhi::ShaderDesc fullscreen_fragment() {
    return {rhi::ShaderStage::Fragment, detail::fullscreen_fragment, "fragment_main"};
}

// PBR-Materialien im Vorwärts-Pass (pbr.slang). Seit R3.5 GPU-getrieben (driven.slang): Push
// Constants wie render::detail::DrivenPush, die Instanz kommt als erste Instanz des Draws
// (Rückfallpfad) bzw. über den Task-Shader (pbr_mesh()); der Frame wie render::GpuFrame.
rhi::ShaderDesc pbr_vertex() {
    return {rhi::ShaderStage::Vertex, detail::pbr_vertex, "vertex_main"};
}

rhi::ShaderDesc pbr_fragment() {
    return {rhi::ShaderStage::Fragment, detail::pbr_fragment, "fragment_main"};
}

// Schattenkarten der Sonne (shadow.slang): nur Tiefe. Push Constants wie render::detail::DrivenPush
// mit der Kaskade. Den Fragment-Shader brauchen nur Materialien mit Alpha-Maske.
rhi::ShaderDesc shadow_vertex() {
    return {rhi::ShaderStage::Vertex, detail::shadow_vertex, "vertex_main"};
}

rhi::ShaderDesc shadow_fragment() {
    return {rhi::ShaderStage::Fragment, detail::shadow_fragment, "fragment_main"};
}

// Debug-Linien (debug.slang), sechs Eckpunkte pro Linie. Push Constants: Adressen der Linien und
// des Frames (je 8), Größe des Ziels in Pixeln (2 × f32), Breite in Pixeln (f32).
rhi::ShaderDesc debug_vertex() {
    return {rhi::ShaderStage::Vertex, detail::debug_vertex, "vertex_main"};
}

rhi::ShaderDesc debug_fragment() {
    return {rhi::ShaderStage::Fragment, detail::debug_fragment, "fragment_main"};
}

// ID-Puffer für die Auswahl im Editor (id.slang): schreibt die Kennung der Instanz in ein
// R32Uint-Ziel. Push Constants wie render::detail::DrivenPush (R3.5).
rhi::ShaderDesc id_vertex() {
    return {rhi::ShaderStage::Vertex, detail::id_vertex, "vertex_main"};
}

rhi::ShaderDesc id_fragment() {
    return {rhi::ShaderStage::Fragment, detail::id_fragment, "fragment_main"};
}

// Voxel-Terrain (terrain.slang, R3.1): Netze aus voxel::Vertex mit Beleuchtung wie pbr.slang,
// dazu Schattenkarten und ID-Puffer. Push Constants wie render::detail::DrivenPush (R3.5).
rhi::ShaderDesc terrain_vertex() {
    return {rhi::ShaderStage::Vertex, detail::terrain_vertex, "vertex_main"};
}

rhi::ShaderDesc terrain_fragment() {
    return {rhi::ShaderStage::Fragment, detail::terrain_fragment, "fragment_main"};
}

rhi::ShaderDesc terrain_shadow_vertex() {
    return {rhi::ShaderStage::Vertex, detail::terrain_shadow_vertex, "shadow_vertex_main"};
}

rhi::ShaderDesc terrain_id_vertex() {
    return {rhi::ShaderStage::Vertex, detail::terrain_id_vertex, "id_vertex_main"};
}

rhi::ShaderDesc terrain_id_fragment() {
    return {rhi::ShaderStage::Fragment, detail::terrain_id_fragment, "id_fragment_main"};
}

// Tone Mapping (tonemap.slang) mit dem Vertex-Shader von fullscreen_vertex(). Push Constants:
// Index der HDR-Textur (u32), Belichtung (f32), 1 für eigene sRGB-Kodierung (u32).
rhi::ShaderDesc tonemap_fragment() {
    return {rhi::ShaderStage::Fragment, detail::tonemap_fragment, "fragment_main"};
}

// Editor-UI (ui.slang): Dreiecke aus der Zeichenliste von Dear ImGui. Push Constants: Adresse der
// Eckpunkte (8, wie editor_ui::UiVertex), Skalierung und Verschiebung von Pixeln in
// Clip-Koordinaten (je 2 × f32), Index der Textur und des Samplers, 1 für eigene sRGB-Kodierung
// (je u32).
rhi::ShaderDesc ui_vertex() {
    return {rhi::ShaderStage::Vertex, detail::ui_vertex, "vertex_main"};
}

rhi::ShaderDesc ui_fragment() {
    return {rhi::ShaderStage::Fragment, detail::ui_fragment, "fragment_main"};
}

// Testmuster: Farbverlauf in eine Storage-Textur, Gruppen zu 8×8. Push Constants: Index der
// Storage-Textur, Breite, Höhe (je u32) und Blauanteil (f32).
rhi::ShaderDesc gradient_compute() {
    return {rhi::ShaderStage::Compute, detail::gradient_compute, "compute_main"};
}

// Mesh Shader der Szene (R3.5, driven.slang): je ein Meshlet pro Arbeitsgruppe, ausgewählt von
// cull_task(). Gleiche Ausgabe wie der Vertex-Shader derselben Datei.
rhi::ShaderDesc pbr_mesh() {
    return {rhi::ShaderStage::Mesh, detail::pbr_mesh, "mesh_main"};
}

rhi::ShaderDesc shadow_mesh() {
    return {rhi::ShaderStage::Mesh, detail::shadow_mesh, "mesh_main"};
}

rhi::ShaderDesc id_mesh() {
    return {rhi::ShaderStage::Mesh, detail::id_mesh, "mesh_main"};
}

rhi::ShaderDesc terrain_mesh() {
    return {rhi::ShaderStage::Mesh, detail::terrain_mesh, "mesh_main"};
}

rhi::ShaderDesc terrain_shadow_mesh() {
    return {rhi::ShaderStage::Mesh, detail::terrain_shadow_mesh, "shadow_mesh_main"};
}

rhi::ShaderDesc terrain_id_mesh() {
    return {rhi::ShaderStage::Mesh, detail::terrain_id_mesh, "id_mesh_main"};
}

// Prüfen der Meshlets (cull.slang, R3.5): Task-Shader für den Mesh-Shader-Pfad, Compute-Shader
// für den Rückfallpfad mit indirekten Draws. Gruppen zu 32 Meshlets.
rhi::ShaderDesc cull_task() {
    return {rhi::ShaderStage::Task, detail::cull_task, "cull_task"};
}

rhi::ShaderDesc cull_compute() {
    return {rhi::ShaderStage::Compute, detail::cull_compute, "cull_compute"};
}

// Hi-Z (hiz.slang, R3.5): eine Stufe der Tiefenpyramide aus der Tiefe oder der Stufe davor,
// Gruppen zu 8×8. Push Constants wie render::detail::HizPush.
rhi::ShaderDesc hiz_compute() {
    return {rhi::ShaderStage::Compute, detail::hiz_compute, "compute_main"};
}

} // namespace ngine::shaders
