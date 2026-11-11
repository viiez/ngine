module;

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module ngine.voxel:streaming;

import ngine.core;
import ngine.math;
import ngine.jobs;
import :grid;
import :chunk;
import :mesh;
import :generator;
import :lod;

export namespace ngine::voxel {

struct StreamDesc {
    LodDesc lod;
    // Höchstens so viele Jobs des Streamings gleichzeitig (Erzeugen, Meshen); 0 heißt die Hälfte
    // der Threads, damit Hauptthread und Grafiktreiber genug Rechenzeit behalten.
    u32 max_generating = 0;
    // Neue Auswahl erst, wenn sich ein Beobachter so weit (Voxel) bewegt hat.
    f64 reselect_distance = 4.0;
};

// Messwerte des Streamers.
struct StreamStats {
    // Blätter der sichtbaren Auswahl, gesamt und pro Stufe.
    u32 leaves = 0;
    std::array<u32, max_lod_level + 1> per_level{};
    // Chunks der laufenden Änderung: noch zu erzeugen, gerade in Jobs, fertig.
    u32 queued = 0;
    u32 generating = 0;
    u32 generated = 0;
    // Abgeschlossene Änderungen der Auswahl und was die letzte gebracht hat.
    u64 commits = 0;
    u32 last_added = 0;
    u32 last_removed = 0;
    u32 last_meshed = 0;
    // Zeit der letzten Übernahme im Hauptthread, in Millisekunden. Die Netze dafür entstehen vorher
    // in Jobs; last_mesh_ms ist die Zeit vom Start dieser Jobs bis zur Übernahme.
    f64 last_commit_ms = 0.0;
    f64 max_commit_ms = 0.0;
    f64 last_mesh_ms = 0.0;
    // Seit dem Start erzeugte Chunks und ihre Zeit in Jobs (Summe über alle Threads).
    u64 generated_total = 0;
    f64 generate_ms_total = 0.0;
    // Speicher der Samples aller Chunks im Speicher (sichtbar und fertig erzeugt).
    usize sample_bytes = 0;
};

// Was update() an den Netzen geändert hat: neue Fassungen und verschwundene Blätter.
struct StreamChanges {
    std::vector<std::pair<ChunkCoord, Mesh>> meshes;
    std::vector<ChunkCoord> removed;

    [[nodiscard]] bool empty() const noexcept {
        return meshes.empty() && removed.empty();
    }
};

// Lädt Terrain um Beobachter (WORLD-08, R3.3, ADR 0041): wählt den Baum der Chunks, erzeugt
// fehlende Chunks im Job-System (Nahes zuerst, höchstens max_generating gleichzeitig). Sind alle
// Chunks einer neuen Auswahl da, meshen Jobs die neuen Blätter und die Nachbarn, deren
// Nachbarschaft sich ändert; erst dann ersetzt es in einem Schritt Chunks und Netze. So gibt es nie
// einen Frame mit Rissen, und der Hauptthread wartet nicht auf das Meshing. Änderungen über Pinsel an Chunks der
// Stufe 0 (ChunkStore::mark_changed) werden im selben Aufruf neu gemeshed. Speicher bleibt
// begrenzt: Im ChunkStore liegen nur die Blätter der Auswahl.
//
// Gehört einem Thread; erzeugende Jobs schreiben nur in eigene Chunks außerhalb des ChunkStore.
// Der Generator ist eine reine Funktion, also brauchen die Jobs keine Sperre.
class Streamer {
public:
    Streamer(const GeneratorDesc& generator, const StreamDesc& desc);
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    // Ein Schritt, einmal pro Frame. observers in Voxeln relativ zum Ursprung des Terrains. Mit
    // wait wartet der Aufruf, bis die Auswahl zu den Beobachtern passt (Tests, Bilder, Server
    // beim Start). Liefert die geänderten Netze.
    StreamChanges update(jobs::JobSystem& jobs, ChunkStore& store,
                         std::span<const math::DVec3> observers, bool wait = false);

    [[nodiscard]] const LodLayout& layout() const noexcept {
        return layout_;
    }
    [[nodiscard]] const StreamStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] const StreamDesc& desc() const noexcept {
        return desc_;
    }
    // Keine Änderung der Auswahl in Arbeit.
    [[nodiscard]] bool idle() const noexcept;

private:
    struct Pending;
    // Schritte einer Änderung der Auswahl. Was nicht an Pinseln und dem ChunkStore hängt, läuft in
    // Jobs; der Hauptthread schaltet nur weiter.
    enum class Phase : u8 { idle, selecting, generating, preparing, meshing };
    void start_batch(const ChunkStore& store);
    void launch(jobs::JobSystem& jobs);
    void collect();
    void prepare();
    void start_meshing(jobs::JobSystem& jobs, const ChunkStore& store);
    [[nodiscard]] u32 job_limit(const jobs::JobSystem& jobs) const noexcept;
    StreamChanges commit(jobs::JobSystem& jobs, ChunkStore& store);
    std::vector<ChunkCoord> edit_targets(ChunkStore& store);
    void mesh_into(jobs::JobSystem& jobs, const ChunkStore& store, std::vector<ChunkCoord> coords,
                   StreamChanges& changes);

    Generator generator_;
    StreamDesc desc_;
    LodLayout layout_;
    // Nachbarschaft, mit der jedes Blatt zuletzt gemeshed wurde.
    std::unordered_map<ChunkCoord, std::vector<ChunkCoord>, ChunkCoordHash> neighbourhoods_;

    // Laufende Änderung der Auswahl: Auswahl in einem Job, dann Chunks erzeugen, dann in einem Job
    // planen, was neu zu meshen ist, dann meshen.
    Phase phase_ = Phase::idle;
    LodLayout planned_;
    LodLayout target_;
    std::vector<ChunkCoord> queue_;
    usize next_ = 0;
    std::vector<std::unique_ptr<Pending>> in_flight_;
    std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash> ready_;
    // Plan aus prepare(): neu zu meshende Blätter mit ihrer neuen Nachbarschaft, alle Chunks, die
    // ihre Netze lesen, verschwindende Blätter.
    std::vector<ChunkCoord> remesh_;
    std::vector<std::vector<ChunkCoord>> remesh_neighbourhoods_;
    std::vector<ChunkCoord> needed_;
    std::vector<ChunkCoord> removed_;
    usize added_ = 0;
    std::vector<Mesh> meshes_;
    std::atomic<usize> next_mesh_{0};
    // Spiegel der Blätter der Stufe 0 im Store: Pinsel ändern diese im Hauptthread, während Jobs
    // meshen, also lesen die Jobs den Spiegel. Neue Blätter spiegeln Jobs beim Meshing; geänderte
    // (stale_) kopiert der Hauptthread vor dem nächsten Meshing neu.
    std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash> mirror_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> stale_;
    usize mirror_bytes_ = 0;
    // Zuordnung aller Chunks, die die Jobs lesen.
    ChunkMap source_;
    // Änderungen über Pinsel, während die Jobs meshen: Ihre Netze kommen nach der Übernahme noch
    // einmal, weil die Jobs mit den alten Samples rechnen.
    std::vector<ChunkCoord> edited_while_meshing_;
    f64 mesh_start_ms_ = 0.0;
    jobs::Counter counter_;
    jobs::JobSystem* jobs_ = nullptr;

    std::vector<math::DVec3> selected_for_;
    StreamStats stats_;
};

} // namespace ngine::voxel
