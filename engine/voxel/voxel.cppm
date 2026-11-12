// Glattes Voxel-Terrain (ADR 0005, ADR 0021). Läuft ohne GPU, damit auch der Server Terrain
// halten und bearbeiten kann.
//   :grid     Dichtefeld mit Material pro Sample, Erzeugung aus einer Funktion
//   :chunk    Chunks mit 32³ Samples, ChunkStore, Randschicht für das Meshing (R3.1)
//   :mesh     Dreiecksnetz aus dem Dichtefeld (Surface Nets)
//   :meshing  Füllen und Meshing vieler Chunks im Job-System (R3.1)
//   :edit     Pinsel zum Abtragen, Auftragen und Glätten, Strahltest, über Chunk-Grenzen
//   :noise    deterministisches Simplex-Rauschen, fBm, Gratrauschen (R3.2)
//   :generator prozedurale Grundwelt aus Regeln: Hügel, Felswände, Höhlen, Überhänge (R3.2)
//   :lod      Baum der Chunks in Stufen um Beobachter, Netze mit Nähten zwischen Stufen (R3.3)
//   :delta    Änderungen als Deltas zur Grundwelt, in allen Stufen, Regionen-Dateien (R3.4)
//   :streaming Laden, Erzeugen und Entladen um Beobachter mit Budget, Übernahme ohne Risse (R3.3)
export module ngine.voxel;

// Positionen sind math::Vec3; wer das Voxel-Modul nutzt, rechnet mit den Funktionen von ngine.math.
export import ngine.math;

export import :grid;
export import :chunk;
export import :mesh;
export import :meshing;
export import :edit;
export import :noise;
export import :generator;
export import :lod;
export import :delta;
export import :streaming;
