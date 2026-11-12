module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module ngine.voxel;

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.profile;

namespace ngine::voxel {

namespace {

f64 elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

f64 now_ms() {
    return std::chrono::duration<f64, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

// Ein Chunk, den ein Job erzeugt; liegt auf dem Heap, damit seine Adresse fest bleibt.
struct Streamer::Pending {
    ChunkCoord coord;
    Chunk chunk;
    f64 ms = 0.0;
    std::atomic<bool> done{false};
};

Streamer::Streamer(const GeneratorDesc& generator, const StreamDesc& desc)
    : generator_{generator}, desc_{desc} {}

Streamer::~Streamer() {
    // Jobs schreiben in eigene Chunks dieses Objekts. Der Destruktor des Job-Systems arbeitet
    // offene Jobs ab; lebt es noch, warten wir hier.
    if (jobs_ != nullptr && !counter_.done()) {
        jobs_->wait(counter_);
    }
}

u32 Streamer::job_limit(const jobs::JobSystem& jobs) const noexcept {
    return desc_.max_generating > 0 ? desc_.max_generating
                                    : std::max(1u, (jobs.worker_count() + 1) / 2);
}

bool Streamer::idle() const noexcept {
    return phase_ == Phase::idle;
}

StreamChanges Streamer::update(jobs::JobSystem& jobs, ChunkStore& store,
                               std::span<const math::DVec3> observers, bool wait) {
    NGINE_PROFILE_ZONE("voxel::Streamer::update");
    jobs_ = &jobs;
    StreamChanges changes;
    // Jeder Schritt wartet auf Jobs nur mit wait; sonst geht es im nächsten Aufruf weiter.
    const auto jobs_done = [&] {
        if (wait) {
            jobs.wait(counter_);
        }
        return counter_.done();
    };
    for (int round = 0; round < 64; ++round) {
        if (phase_ == Phase::idle) {
            bool moved = selected_for_.size() != observers.size();
            for (usize i = 0; !moved && i < observers.size(); ++i) {
                moved = math::length(observers[i] - selected_for_[i]) > desc_.reselect_distance;
            }
            if (!moved) {
                break;
            }
            // Die Auswahl rechnet ein Job; selected_for_ bleibt bis zur nächsten unverändert.
            selected_for_.assign(observers.begin(), observers.end());
            phase_ = Phase::selecting;
            jobs.run([this] { planned_ = select_lod(selected_for_, desc_.lod); }, &counter_);
        }
        if (phase_ == Phase::selecting) {
            if (!jobs_done()) {
                break;
            }
            if (planned_ == layout_) {
                planned_ = {};
                phase_ = Phase::idle;
                continue;
            }
            target_ = std::move(planned_);
            planned_ = {};
            start_batch(store);
        }
        if (phase_ == Phase::generating) {
            collect();
            launch(jobs);
            while (wait && !in_flight_.empty()) {
                jobs.wait(counter_);
                collect();
                launch(jobs);
            }
            if (next_ < queue_.size() || !in_flight_.empty()) {
                break;
            }
            phase_ = Phase::preparing;
            jobs.run([this] { prepare(); }, &counter_);
        }
        if (phase_ == Phase::preparing) {
            if (!jobs_done()) {
                break;
            }
            start_meshing(jobs, store);
        }
        if (!jobs_done()) {
            break;
        }
        StreamChanges committed = commit(jobs, store);
        // Zwei Übernahmen in einem Aufruf (mit wait): Netze von Blättern, die die zweite entfernt,
        // fallen weg.
        std::erase_if(changes.meshes, [&](const auto& entry) {
            return std::find(committed.removed.begin(), committed.removed.end(), entry.first) !=
                   committed.removed.end();
        });
        for (auto& entry : committed.meshes) {
            changes.meshes.push_back(std::move(entry));
        }
        changes.removed.insert(changes.removed.end(), committed.removed.begin(),
                               committed.removed.end());
        // Weiter mit der nächsten Auswahl, falls sich die Beobachter inzwischen bewegt haben.
    }
    if (store.has_changes()) {
        mesh_into(jobs, store, edit_targets(store), changes);
    }
    stats_.queued = static_cast<u32>(queue_.size() - next_);
    stats_.generating = static_cast<u32>(in_flight_.size());
    stats_.generated = static_cast<u32>(ready_.size());
    usize bytes = store.memory_bytes();
    for (const auto& [coord, chunk] : ready_) {
        bytes += chunk.memory_bytes();
    }
    // Den Spiegel füllen eventuell gerade Jobs; gezählt wird er bei der Übernahme.
    stats_.sample_bytes = bytes + mirror_bytes_;
    return changes;
}

void Streamer::start_batch(const ChunkStore& store) {
    phase_ = Phase::generating;
    queue_.clear();
    next_ = 0;
    ready_.clear();
    for (const ChunkCoord c : target_.leaves()) {
        if (!layout_.contains(c) || store.find(c) == nullptr) {
            queue_.push_back(c);
        }
    }
    // Nahes zuerst; bei gleichem Abstand feine Stufen zuerst.
    std::vector<std::pair<f64, ChunkCoord>> order;
    order.reserve(queue_.size());
    for (const ChunkCoord c : queue_) {
        order.emplace_back(observer_distance(selected_for_, c), c);
    }
    std::sort(order.begin(), order.end());
    for (usize i = 0; i < order.size(); ++i) {
        queue_[i] = order[i].second;
    }
}

void Streamer::launch(jobs::JobSystem& jobs) {
    const u32 limit = job_limit(jobs);
    while (in_flight_.size() < limit && next_ < queue_.size()) {
        auto pending = std::make_unique<Pending>();
        pending->coord = queue_[next_++];
        Pending* job = pending.get();
        const Generator* generator = &generator_;
        jobs.run(
            [generator, job] {
                const auto start = std::chrono::steady_clock::now();
                generator->fill(job->coord, job->chunk);
                job->ms = elapsed_ms(start);
                job->done.store(true, std::memory_order_release);
            },
            &counter_);
        in_flight_.push_back(std::move(pending));
    }
}

void Streamer::collect() {
    for (usize i = 0; i < in_flight_.size();) {
        Pending& pending = *in_flight_[i];
        if (!pending.done.load(std::memory_order_acquire)) {
            ++i;
            continue;
        }
        stats_.generate_ms_total += pending.ms;
        ++stats_.generated_total;
        ready_.insert_or_assign(pending.coord, std::move(pending.chunk));
        in_flight_[i] = std::move(in_flight_.back());
        in_flight_.pop_back();
    }
}

void Streamer::mesh_into(jobs::JobSystem& jobs, const ChunkStore& store,
                         std::vector<ChunkCoord> coords, StreamChanges& changes) {
    std::sort(coords.begin(), coords.end());
    coords.erase(std::unique(coords.begin(), coords.end()), coords.end());
    std::vector<Mesh> meshes(coords.size());
    mesh_leaves(jobs, layout_, store, coords, meshes);
    for (usize i = 0; i < coords.size(); ++i) {
        changes.meshes.emplace_back(coords[i], std::move(meshes[i]));
    }
}

// Läuft in einem Job: liest layout_, target_ und neighbourhoods_, die der Hauptthread bis zur
// Übernahme nur liest.
void Streamer::prepare() {
    NGINE_PROFILE_ZONE("voxel::Streamer::prepare");
    // Neu zu meshen: neue Blätter und Blätter, deren Nachbarschaft sich ändert. Das können nur
    // Nachbarn neuer oder verschwindender Blätter sein.
    std::unordered_set<ChunkCoord, ChunkCoordHash> candidates;
    added_ = 0;
    for (const ChunkCoord c : target_.leaves()) {
        if (!layout_.contains(c)) {
            ++added_;
            for (const ChunkCoord n : target_.neighbourhood(c)) {
                candidates.insert(n);
            }
        }
    }
    removed_.clear();
    for (const ChunkCoord c : layout_.leaves()) {
        if (!target_.contains(c)) {
            removed_.push_back(c);
            for (const ChunkCoord n : layout_.neighbourhood(c)) {
                if (target_.contains(n)) {
                    candidates.insert(n);
                }
            }
        }
    }
    remesh_.clear();
    remesh_neighbourhoods_.clear();
    std::unordered_set<ChunkCoord, ChunkCoordHash> needed;
    for (const ChunkCoord c : candidates) {
        std::vector<ChunkCoord> around = target_.neighbourhood(c);
        const auto it = neighbourhoods_.find(c);
        if (it == neighbourhoods_.end() || it->second != around) {
            needed.insert(around.begin(), around.end());
            remesh_.push_back(c);
            remesh_neighbourhoods_.push_back(std::move(around));
        }
    }
    needed_.assign(needed.begin(), needed.end());
}

void Streamer::start_meshing(jobs::JobSystem& jobs, const ChunkStore& store) {
    NGINE_PROFILE_ZONE("voxel::Streamer::start_meshing");
    phase_ = Phase::meshing;
    mesh_start_ms_ = now_ms();
    // Was die Jobs lesen: neue Chunks aus ready_, grobe Chunks direkt aus dem Store (Pinsel ändern
    // nur Stufe 0), Chunks der Stufe 0 aus dem Spiegel, weil Pinsel sie im Hauptthread weiter
    // ändern. ready_ und target_ bleiben bis zur Übernahme unverändert.
    source_.clear();
    source_.set_outside(store.outside());
    for (const ChunkCoord n : needed_) {
        if (const auto it = ready_.find(n); it != ready_.end()) {
            source_.add(n, &it->second);
        } else if (const Chunk* chunk = store.find(n)) {
            if (n.level != 0) {
                source_.add(n, chunk);
                continue;
            }
            // Meist schon gespiegelt; kopiert wird nur, was fehlt oder sich geändert hat.
            const auto [mirrored, inserted] = mirror_.try_emplace(n);
            const bool stale = stale_.erase(n) > 0;
            if (inserted || stale) {
                mirrored->second = *chunk;
            }
            source_.add(n, &mirrored->second);
        }
    }
    // Neue Blätter der Stufe 0 spiegeln Jobs, solange ready_ unverändert bleibt; die Einträge legt
    // der Hauptthread an, damit die Jobs nur in eigene Chunks schreiben.
    std::vector<std::pair<Chunk*, const Chunk*>> to_mirror;
    for (const auto& [coord, chunk] : ready_) {
        if (coord.level == 0) {
            stale_.erase(coord);
            to_mirror.emplace_back(&mirror_[coord], &chunk);
        }
    }
    constexpr usize mirror_per_job = 32;
    for (usize begin = 0; begin < to_mirror.size(); begin += mirror_per_job) {
        const usize end = std::min(to_mirror.size(), begin + mirror_per_job);
        jobs.run(
            [part = std::vector(to_mirror.begin() + static_cast<std::ptrdiff_t>(begin),
                                to_mirror.begin() + static_cast<std::ptrdiff_t>(end))] {
                for (const auto& [mirror, chunk] : part) {
                    *mirror = *chunk;
                }
            },
            &counter_);
    }

    meshes_.clear();
    meshes_.resize(remesh_.size());
    // Höchstens job_limit() Jobs, die sich die Blätter teilen: Das Streaming soll nicht alle
    // Threads belegen, sonst fehlt dem Hauptthread und dem Grafiktreiber Rechenzeit.
    next_mesh_.store(0, std::memory_order_relaxed);
    const usize count = std::min<usize>(job_limit(jobs), remesh_.size());
    for (usize j = 0; j < count; ++j) {
        jobs.run(
            [this] {
                LodMesher mesher;
                for (usize i = next_mesh_.fetch_add(1, std::memory_order_relaxed);
                     i < remesh_.size(); i = next_mesh_.fetch_add(1, std::memory_order_relaxed)) {
                    mesher.build(target_, source_, remesh_[i], meshes_[i]);
                }
            },
            &counter_);
    }
}

StreamChanges Streamer::commit(jobs::JobSystem& jobs, ChunkStore& store) {
    NGINE_PROFILE_ZONE("voxel::Streamer::commit");
    const auto start = std::chrono::steady_clock::now();
    StreamChanges changes;
    layout_ = std::move(target_);
    target_ = {};
    phase_ = Phase::idle;

    changes.removed = std::move(removed_);
    removed_.clear();
    for (const ChunkCoord c : changes.removed) {
        neighbourhoods_.erase(c);
        store.remove(c);
        mirror_.erase(c);
        stale_.erase(c);
    }
    for (auto& [coord, chunk] : ready_) {
        store.get_or_create(coord) = std::move(chunk);
    }
    ready_.clear();
    queue_.clear();
    next_ = 0;
    source_.clear();
    needed_.clear();
    // Im Store liegen damit nur Blätter der Auswahl: Chunks, die Pinsel außerhalb anlegen, entfernt
    // edit_targets() schon im selben Aufruf.

    // Die Netze aus den Jobs.
    changes.meshes.reserve(remesh_.size());
    for (usize i = 0; i < remesh_.size(); ++i) {
        neighbourhoods_.insert_or_assign(remesh_[i], std::move(remesh_neighbourhoods_[i]));
        changes.meshes.emplace_back(remesh_[i], std::move(meshes_[i]));
    }
    remesh_.clear();
    remesh_neighbourhoods_.clear();
    meshes_.clear();
    // Änderungen über Pinsel seit dem Start der Jobs gleich mit; ihre Netze kommen nach denen aus
    // den Jobs und ersetzen sie.
    for (const ChunkCoord c : edited_while_meshing_) {
        store.mark_changed(c);
    }
    edited_while_meshing_.clear();
    if (store.has_changes()) {
        mesh_into(jobs, store, edit_targets(store), changes);
    }

    mirror_bytes_ = 0;
    for (const auto& [coord, chunk] : mirror_) {
        mirror_bytes_ += chunk.memory_bytes();
    }
    ++stats_.commits;
    stats_.leaves = static_cast<u32>(layout_.size());
    stats_.per_level = layout_.level_counts();
    stats_.last_added = static_cast<u32>(added_);
    stats_.last_removed = static_cast<u32>(changes.removed.size());
    stats_.last_meshed = static_cast<u32>(changes.meshes.size());
    stats_.last_commit_ms = elapsed_ms(start);
    stats_.max_commit_ms = std::max(stats_.max_commit_ms, stats_.last_commit_ms);
    stats_.last_mesh_ms = now_ms() - mesh_start_ms_;
    return changes;
}

std::vector<ChunkCoord> Streamer::edit_targets(ChunkStore& store) {
    std::unordered_set<ChunkCoord, ChunkCoordHash> touched;
    const std::vector<ChunkCoord> changed = store.take_changes();
    // Der Spiegel dieser Chunks ist veraltet; neu kopiert wird beim nächsten Meshing.
    for (const ChunkCoord c : changed) {
        if (c.level == 0 && mirror_.contains(c)) {
            stale_.insert(c);
        }
    }
    if (phase_ == Phase::meshing) {
        edited_while_meshing_.insert(edited_while_meshing_.end(), changed.begin(), changed.end());
    }
    for (const ChunkCoord c : changed) {
        // Pinsel legen Chunks der Stufe 0 auch dort an, wo ein grobes Blatt liegt; die gehören
        // nicht zur Auswahl (Änderungen in groben Stufen folgen mit den Deltas in R3.4).
        if (store.find(c) != nullptr && !layout_.contains(c)) {
            store.remove(c);
        }
        if (const auto leaf = layout_.leaf_of(chunk_origin(c.x), chunk_origin(c.y),
                                              chunk_origin(c.z))) {
            touched.insert(*leaf);
        }
    }
    // Das Netz eines Blatts liest nur seine Nachbarschaft; ein geändertes Blatt betrifft also sich
    // und seine Nachbarn.
    std::vector<ChunkCoord> remesh;
    for (const ChunkCoord leaf : touched) {
        for (const ChunkCoord n : layout_.neighbourhood(leaf)) {
            remesh.push_back(n);
        }
    }
    return remesh;
}

} // namespace ngine::voxel
