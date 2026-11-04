// Renderer der Engine (Laufzeit-Schicht). Er kennt nur die RHI, nie die Grafik-API dahinter
// (ADR 0004, ADR 0026).
//   :pool   transiente Texturen, über Frames hinweg wiederverwendet
//   :graph  Render-Graph: Passes, Ressourcen, Barrieren
export module ngine.render;

export import :pool;
export import :graph;
