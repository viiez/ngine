// Renderer der Engine (Laufzeit-Schicht). Er kennt nur die RHI, nie die Grafik-API dahinter
// (ADR 0004, ADR 0026).
//   :pool        transiente Texturen, über Frames hinweg wiederverwendet
//   :profiler    GPU-Zonen in Tracy über Zeitstempel
//   :graph       Render-Graph: Passes, Ressourcen, Barrieren
//   :components  Camera, MeshRenderer und Lichter für die Welt
//   :lights      Lichter der Welt als Liste für die GPU
//   :shadows     Kaskaden der Schatten: Aufteilung, Einpassen, Einrasten
//   :debug_draw  Linien, Boxen und Kugeln zur Fehlersuche
//   :materials   Materialien, Texturen und Sampler (MaterialStore)
//   :geometry    gemeinsame Geometriepuffer mit Verwaltung freier Bereiche (TD-23)
//   :meshes      Geometrie auf der GPU (MeshStore)
//   :culling     Daten und Rechnungen für das GPU-getriebene Zeichnen mit Meshlets (R3.5)
//   :terrain     Netze der Chunks aller Terrains auf der GPU (R3.1), Materialtabelle (R3.2)
//   :renderer    zeichnet die Welt: Schatten, PBR vorwärts in ein HDR-Ziel, Tone Mapping; seit R3.1
//                auch Terrain aus Chunks, seit R3.2 mit Materialien und Texturen, seit R3.5
//                GPU-getrieben mit Meshlets und Hi-Z
// Dazu die interne Partition :renderer_detail mit dem Innenleben des Renderers, nur für
// renderer.cpp, pipelines.cpp, passes.cpp, terrain_pass.cpp, textures.cpp und models.cpp (TD-36).
export module ngine.render;

export import :pool;
export import :profiler;
export import :graph;
export import :components;
export import :lights;
export import :shadows;
export import :debug_draw;
export import :materials;
export import :geometry;
export import :meshes;
export import :culling;
export import :terrain;
export import :renderer;
