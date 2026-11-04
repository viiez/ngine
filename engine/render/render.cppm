// Renderer der Engine (Laufzeit-Schicht). Er kennt nur die RHI, nie die Grafik-API dahinter
// (ADR 0004, ADR 0026).
//   :pool        transiente Texturen, über Frames hinweg wiederverwendet
//   :graph       Render-Graph: Passes, Ressourcen, Barrieren
//   :components  Camera und MeshRenderer für die Welt
//   :materials   Materialien, Texturen und Sampler (MaterialStore)
//   :meshes      Geometrie auf der GPU (MeshStore)
//   :renderer    zeichnet die Welt: PBR vorwärts in ein HDR-Ziel, Tone Mapping
export module ngine.render;

export import :pool;
export import :graph;
export import :components;
export import :materials;
export import :meshes;
export import :renderer;
