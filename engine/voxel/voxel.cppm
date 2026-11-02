// Glattes Voxel-Terrain (ADR 0005, ADR 0021). Läuft ohne GPU, damit auch der Server Terrain
// halten und bearbeiten kann.
//   :grid  Dichtefeld mit Material pro Sample, Erzeugung aus einer Funktion
//   :mesh  Dreiecksnetz aus dem Dichtefeld (Surface Nets)
//   :edit  Pinsel zum Abtragen, Auftragen und Glätten, Strahltest
export module ngine.voxel;

export import :grid;
export import :mesh;
export import :edit;
