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

constexpr u32 gradient_compute[] = {
#include "gradient_compute.spv.inc"
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

// Testmuster: Farbverlauf in eine Storage-Textur, Gruppen zu 8×8. Push Constants: Index der
// Storage-Textur, Breite, Höhe (je u32) und Blauanteil (f32).
rhi::ShaderDesc gradient_compute() {
    return {rhi::ShaderStage::Compute, detail::gradient_compute, "compute_main"};
}

} // namespace ngine::shaders
