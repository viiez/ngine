module;

#include <ngine/profile.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

// Demo für Job-System und Speicher: Pro Frame wird ein Graph aus vielen kleinen Aufgaben
// ausgeführt, ähnlich dem späteren Voxel-Meshing. Erst mit einem, dann mit allen Kernen, damit
// die Beschleunigung sichtbar wird. In Tracy erscheinen die Worker mit ihren Zonen.
export module sandbox.jobs_demo;

import ngine.core;
import ngine.memory;
import ngine.jobs;
import ngine.profile;

namespace sandbox {

namespace {

using namespace ngine;

constexpr u32 chunk_count = 64;
constexpr u32 chunk_size = 32;
constexpr usize samples_per_chunk = usize{chunk_size} * chunk_size * chunk_size;

// Platz für die Dichtewerte aller Chunks eines Frames, plus etwas Reserve.
constexpr usize frame_bytes = chunk_count * samples_per_chunk * sizeof(f32) + 64 * 1024;

struct Chunk {
    std::span<f32> density;
    u32 surface_cells = 0;
};

// Ein glatter Hügel mit Wellen: genug Rechenarbeit, um Jobs messbar zu machen.
f32 density_at(u32 chunk, u32 x, u32 y, u32 z, f32 time) {
    const f32 wx = static_cast<f32>(chunk % 8 * chunk_size + x) * 0.05f;
    const f32 wz = static_cast<f32>(chunk / 8 * chunk_size + z) * 0.05f;
    const f32 height = 16.0f + 6.0f * std::sin(wx + time) * std::cos(wz - time);
    return height - static_cast<f32>(y);
}

void generate(Chunk& chunk, u32 index, f32 time) {
    NGINE_PROFILE_ZONE("Dichte erzeugen");
    usize i = 0;
    for (u32 z = 0; z < chunk_size; ++z) {
        for (u32 y = 0; y < chunk_size; ++y) {
            for (u32 x = 0; x < chunk_size; ++x) {
                chunk.density[i++] = density_at(index, x, y, z, time);
            }
        }
    }
}

// Zählt Zellen, in denen die Dichte das Vorzeichen wechselt: dort läge später die Oberfläche.
void find_surface(Chunk& chunk) {
    NGINE_PROFILE_ZONE("Oberfläche suchen");
    const auto at = [&](u32 x, u32 y, u32 z) {
        return chunk.density[(usize{z} * chunk_size + y) * chunk_size + x];
    };
    u32 cells = 0;
    for (u32 z = 0; z + 1 < chunk_size; ++z) {
        for (u32 y = 0; y + 1 < chunk_size; ++y) {
            for (u32 x = 0; x + 1 < chunk_size; ++x) {
                const bool inside = at(x, y, z) > 0.0f;
                if (inside != (at(x + 1, y, z) > 0.0f) || inside != (at(x, y + 1, z) > 0.0f) ||
                    inside != (at(x, y, z + 1) > 0.0f)) {
                    ++cells;
                }
            }
        }
    }
    chunk.surface_cells = cells;
}

struct Result {
    std::chrono::duration<f64, std::milli> average_frame;
    u64 surface_cells = 0;
};

Result run(std::optional<u32> workers, u64 frames) {
    jobs::JobSystem job_system{{.worker_count = workers}};
    memory::FrameAllocator frame_memory{frame_bytes, 2, memory::Category::World};

    std::vector<Chunk> chunks(chunk_count);
    f32 time = 0.0f;
    u64 total_cells = 0;

    // Der Graph entsteht einmal und läuft jeden Frame: Dichte, dann Oberfläche, dann Summe.
    jobs::TaskGraph graph;
    const auto sum = graph.add([&] {
        NGINE_PROFILE_ZONE("Summieren");
        for (const Chunk& chunk : chunks) {
            total_cells += chunk.surface_cells;
        }
    });
    for (u32 i = 0; i < chunk_count; ++i) {
        const auto generate_task = graph.add([&chunks, &time, i] { generate(chunks[i], i, time); });
        const auto surface_task = graph.add([&chunks, i] { find_surface(chunks[i]); });
        graph.precede(generate_task, surface_task);
        graph.precede(surface_task, sum);
    }

    const auto start = std::chrono::steady_clock::now();
    for (u64 frame = 0; frame < frames; ++frame) {
        NGINE_PROFILE_ZONE("Frame");
        frame_memory.next_frame();
        for (Chunk& chunk : chunks) {
            auto* data = static_cast<f32*>(
                frame_memory.allocate(samples_per_chunk * sizeof(f32), alignof(f32)));
            chunk.density = {data, samples_per_chunk};
        }
        graph.run_and_wait(job_system);
        time += 0.016f;
        profile::plot("Frame-Speicher (Bytes)", static_cast<f64>(frame_memory.used()));
        profile::frame_mark();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return Result{
        .average_frame = elapsed / static_cast<f64>(frames),
        .surface_cells = total_cells,
    };
}

} // namespace

// Führt die Demo aus und meldet die Zeiten. Liefert den Exit-Code.
export int run_jobs_demo(ngine::u64 frames) {
    using namespace ngine;
    profile::set_thread_name("Hauptthread");
    log::info("Job-Demo: {} Chunks mit {}³ Dichtewerten, {} Frames", chunk_count, chunk_size,
              frames);

    const Result single = run(0u, frames);
    log::info("Ein Thread:     {:.2f} ms pro Frame", single.average_frame.count());

    const Result parallel = run(std::nullopt, frames);
    log::info("Alle Kerne:     {:.2f} ms pro Frame, {:.1f}x schneller",
              parallel.average_frame.count(),
              single.average_frame.count() / parallel.average_frame.count());

    if (single.surface_cells != parallel.surface_cells) {
        log::error("Ergebnisse weichen ab: {} gegen {} Oberflächenzellen", single.surface_cells,
                   parallel.surface_cells);
        return 1;
    }
    const auto stats = memory::stats(memory::Category::World);
    log::info("Speicher Welt: Spitze {} KiB", stats.peak / 1024);
    return 0;
}

} // namespace sandbox
