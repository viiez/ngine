// Ressourcen-Schicht: Modelle und Bilder aus Dateien in reine CPU-Daten (ASSET-01, ASSET-02),
// stabile Kennungen für Quelldateien (ASSET-05) und der Ressourcen-Manager (ASSET-04). Kennt
// weder die GPU noch die Welt; der Renderer lädt die Daten hoch, Sandbox und Editor legen
// Entities an. Die Formate lesen Adapter (fastgltf, stb_image), die außerhalb dieses Moduls
// niemand sieht (ADR 0027, ADR 0033).
export module ngine.asset;

export import :model;
export import :image;
export import :geometry;
export import :database;
export import :resources;
