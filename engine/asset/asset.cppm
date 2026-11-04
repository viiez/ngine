// Ressourcen-Schicht: Modelle und Bilder aus Dateien in reine CPU-Daten (ASSET-01, ASSET-02).
// Kennt weder die GPU noch die Welt; der Renderer lädt die Daten hoch, die Sandbox legt Entities
// an. Die Formate lesen Adapter (fastgltf, stb_image), die außerhalb dieses Moduls niemand sieht
// (ADR 0027).
export module ngine.asset;

export import :model;
export import :image;
export import :geometry;
