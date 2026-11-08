module;

#include <span>

export module ngine.voxel:meshing;

import ngine.core;
import ngine.jobs;
import :grid;
import :chunk;
import :mesh;

export namespace ngine::voxel {

// Netz eines Chunks der Stufe 0 (Positionen relativ zu seiner Ecke). scratch wird zum
// Zwischenspeicher für Samples samt Randschicht; Mesher und scratch gehören einem Thread.
void mesh_chunk(const ChunkStore& store, ChunkCoord coord, Mesher& mesher, Grid& scratch,
                Mesh& out);

// Meshing vieler Chunks parallel im Job-System (VOX-02): out[i] wird zum Netz von coords[i].
// Die Chunks werden nur gelesen; während des Aufrufs darf niemand store ändern.
void mesh_chunks(jobs::JobSystem& jobs, const ChunkStore& store, std::span<const ChunkCoord> coords,
                 std::span<Mesh> out);

// Füllt die Chunks coords (legt sie bei Bedarf an) aus einer Abstandsfunktion in Voxeln,
// parallel im Job-System; jeder Job schreibt nur in seinen Chunk. distance und material bekommen
// die Position in Voxeln relativ zum Ursprung des Terrains und werden gleichzeitig aus mehreren
// Threads aufgerufen. Einheitliche Chunks schrumpfen auf einen Wert; alle betroffenen Netze sind
// danach vorgemerkt.
void fill_chunks(jobs::JobSystem& jobs, ChunkStore& store, std::span<const ChunkCoord> coords,
                 const DistanceFunction& distance, const MaterialFunction& material);

} // namespace ngine::voxel
