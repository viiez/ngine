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
//   :meshes      Geometrie auf der GPU (MeshStore)
//   :renderer    zeichnet die Welt: Schatten, PBR vorwärts in ein HDR-Ziel, Tone Mapping
export module ngine.render;

export import :pool;
export import :profiler;
export import :graph;
export import :components;
export import :lights;
export import :shadows;
export import :debug_draw;
export import :materials;
export import :meshes;
export import :renderer;
