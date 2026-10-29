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

} // namespace ngine::shaders::detail

export namespace ngine::shaders {

rhi::ShaderDesc triangle_vertex() {
    return {rhi::ShaderStage::Vertex, detail::triangle_vertex, "vertex_main"};
}

rhi::ShaderDesc triangle_fragment() {
    return {rhi::ShaderStage::Fragment, detail::triangle_fragment, "fragment_main"};
}

} // namespace ngine::shaders
