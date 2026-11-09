module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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

bool Streamer::idle() const noexcept {
    return !batch_;
}

StreamChanges Streamer::update(jobs::JobSystem& jobs, ChunkStore& store,
                               std::span<const math::DVec3> observers, bool wait) {
    NGINE_PROFILE_ZONE("voxel::Streamer::update");
    jobs_ = &jobs;
    StreamChanges changes;
    for (int round = 0; round < 64; ++round) {
        if (!batch_) {
            bool moved = selected_for_.size() != observers.size();
            for (usize i = 0; !moved && i < observers.size(); ++i) {
                moved = math::length(observers[i] - selected_for_[i]) > desc_.reselect_distance;
            }
            if (moved) {
                selected_for_.assign(observers.begin(), observers.end());
                LodLayout target = select_lod(observers, desc_.lod);
                if (!(target == layout_)) {
                    target_ = std::move(target);
                    start_batch(store, observers);
                }
            }
        }
        if (!batch_) {
            break;
        }
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
    stats_.sample_bytes = bytes;
    return changes;
}

void Streamer::start_batch(const ChunkStore& store, std::span<const math::DVec3> observers) {
    batch_ = true;
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
        order.emplace_back(observer_distance(observers, c), c);
    }
    std::sort(order.begin(), order.end());
    for (usize i = 0; i < order.size(); ++i) {
        queue_[i] = order[i].second;
    }
}

void Streamer::launch(jobs::JobSystem& jobs) {
    const u32 limit =
        desc_.max_generating > 0 ? desc_.max_generating : 2 * (jobs.worker_count() + 1);
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

StreamChanges Streamer::commit(jobs::JobSystem& jobs, ChunkStore& store) {
    NGINE_PROFILE_ZONE("voxel::Streamer::commit");
    const auto start = std::chrono::steady_clock::now();
    StreamChanges changes;
    LodLayout old = std::move(layout_);
    layout_ = std::move(target_);
    target_ = {};
    batch_ = false;

    std::vector<ChunkCoord> added;
    for (const ChunkCoord c : layout_.leaves()) {
        if (!old.contains(c)) {
            added.push_back(c);
        }
    }
    for (const ChunkCoord c : old.leaves()) {
        if (!layout_.contains(c)) {
            changes.removed.push_back(c);
            neighbourhoods_.erase(c);
        }
    }
    for (auto& [coord, chunk] : ready_) {
        store.get_or_create(coord) = std::move(chunk);
    }
    ready_.clear();
    queue_.clear();
    next_ = 0;
    // Nur Blätter der Auswahl bleiben im Speicher (auch keine, die Pinsel außerhalb angelegt
    // haben).
    for (const ChunkCoord c : store.coords()) {
        if (!layout_.contains(c)) {
            store.remove(c);
        }
    }

    // Neu zu meshen: neue Blätter und Blätter, deren Nachbarschaft sich geändert hat. Das können
    // nur Nachbarn neuer oder verschwundener Blätter sein.
    std::unordered_set<ChunkCoord, ChunkCoordHash> candidates;
    for (const ChunkCoord c : added) {
        for (const ChunkCoord n : layout_.neighbourhood(c)) {
            candidates.insert(n);
        }
    }
    for (const ChunkCoord c : changes.removed) {
        for (const ChunkCoord n : old.neighbourhood(c)) {
            if (layout_.contains(n)) {
                candidates.insert(n);
            }
        }
    }
    std::vector<ChunkCoord> remesh;
    for (const ChunkCoord c : candidates) {
        std::vector<ChunkCoord> around = layout_.neighbourhood(c);
        auto [it, inserted] = neighbourhoods_.try_emplace(c);
        if (inserted || it->second != around) {
            it->second = std::move(around);
            remesh.push_back(c);
        }
    }
    // Änderungen über Pinsel seit dem letzten Aufruf gleich mit.
    if (store.has_changes()) {
        const std::vector<ChunkCoord> edited = edit_targets(store);
        remesh.insert(remesh.end(), edited.begin(), edited.end());
    }
    mesh_into(jobs, store, std::move(remesh), changes);

    ++stats_.commits;
    stats_.leaves = static_cast<u32>(layout_.size());
    stats_.per_level = layout_.level_counts();
    stats_.last_added = static_cast<u32>(added.size());
    stats_.last_removed = static_cast<u32>(changes.removed.size());
    stats_.last_meshed = static_cast<u32>(changes.meshes.size());
    stats_.last_commit_ms = elapsed_ms(start);
    stats_.max_commit_ms = std::max(stats_.max_commit_ms, stats_.last_commit_ms);
    return changes;
}

std::vector<ChunkCoord> Streamer::edit_targets(ChunkStore& store) {
    std::unordered_set<ChunkCoord, ChunkCoordHash> touched;
    for (const ChunkCoord c : store.take_changes()) {
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
