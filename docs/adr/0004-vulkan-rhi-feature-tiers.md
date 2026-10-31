# 0004: Vulkan 1.3 hinter einer RHI, Feature-Stufen, Slang

- **Status:** angenommen; Vulkan-Version und Feature-Stufen ersetzt durch
  [0014](0014-modern-gpu-baseline.md)
- **Datum:** 2026-10-27

## Kontext

Vulkan ist die primäre Grafik-API, später sollen weitere dazukommen. Die Testhardware reicht von
integrierter Intel-Grafik (keine Mesh Shader, kein Raytracing) bis zur RTX 3060 Ti.

## Entscheidung

- Vulkan 1.3 mit Dynamic Rendering, Synchronization2 und Bindless.
- Eine schmale RHI (Render Hardware Interface) trennt Renderer und API.
- Feature-Stufen: Die Basis läuft auf der Intel-Grafik; Mesh Shader und Raytracing werden zur
  Laufzeit erkannt und nur genutzt, wo vorhanden.
- Shader in Slang, kompiliert zu SPIR-V; später auch DXIL und Metal.

## Konsequenzen

- D3D12 oder Metal lassen sich ergänzen, ohne den Renderer umzuschreiben.
- Die schwache Grafik zwingt zu sparsamem, skalierbarem Rendering.
- Jedes optionale Feature braucht einen Rückfallpfad.

## Erwogene Alternativen

- **Ältere Vulkan-Versionen mit Render Passes:** mehr Code, keine Vorteile für uns.
- **HLSL mit DXC oder GLSL:** siehe [Fremdbibliotheken](../third-party.md).
