module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.world;

import ngine.core;
import ngine.jobs;
import ngine.profile;

namespace ngine::world {

namespace {

// Zwei Systeme kommen sich in die Quere, wenn eines eine Komponente schreibt, die das andere
// liest oder schreibt. Nur lesen dürfen beliebig viele gleichzeitig.
bool conflicts(const detail::QueryBase& a, const detail::QueryBase& b) {
    for (const detail::Term& x : a.terms()) {
        for (const detail::Term& y : b.terms()) {
            if (x.component == y.component && (!x.read_only || !y.read_only)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

struct Schedule::Impl {
    struct Entry {
        std::string name;
        SystemOptions options;
        std::unique_ptr<detail::System> system;
        // Wird vor jedem Lauf im aufrufenden Thread gefüllt; die Jobs lesen nur.
        std::vector<detail::Chunk> chunks;
        // Erstes Stück jedes Chunks; ein Chunk mit count Zeilen hat pieces(count) Stücke.
        std::vector<usize> first_piece;
        // Ein Befehlspuffer pro Stück, nur bei Systemen mit Befehlspuffer. Jedes Stück schreibt
        // nur in seinen eigenen; die Reihenfolge der Puffer ist die von Chunk und Zeile.
        std::vector<Commands> commands;
        std::vector<usize> waits_for;
    };

    struct PhaseGraph {
        std::vector<usize> systems;
        std::unique_ptr<jobs::TaskGraph> graph;
    };

    // Zahl der Stücke, in die ein System einen Chunk teilt.
    static usize pieces(const Entry& entry, u32 count) noexcept {
        const usize grain = entry.options.grain;
        if (grain == 0 || count <= grain) {
            return 1;
        }
        return (count + grain - 1) / grain;
    }

    void run_piece(Entry& entry, usize chunk_index, usize piece) {
        const detail::Chunk& chunk = entry.chunks[chunk_index];
        const usize grain = pieces(entry, chunk.count) == 1 ? chunk.count : entry.options.grain;
        const auto begin = static_cast<u32>(piece * grain);
        const auto end = static_cast<u32>(std::min<usize>(begin + grain, chunk.count));
        Commands* commands = entry.system->uses_commands()
                                 ? &entry.commands[entry.first_piece[chunk_index] + piece]
                                 : nullptr;
        entry.system->run_rows(chunk, begin, end, commands);
    }

    void run_system(usize index) {
        NGINE_PROFILE_ZONE("System");
        Entry& entry = entries[index];
        for (usize c = 0; c < entry.chunks.size(); ++c) {
            const usize count = pieces(entry, entry.chunks[c].count);
            if (count == 1) {
                run_piece(entry, c, 0);
                continue;
            }
            // Feste Stücke statt der Halbierung von parallel_for: So gehört jedes Stück zu genau
            // einem Befehlspuffer, egal welcher Thread es rechnet.
            jobs->parallel_for(count, 1, [&](usize begin, usize end) {
                for (usize piece = begin; piece < end; ++piece) {
                    run_piece(entry, c, piece);
                }
            });
        }
    }

    void build_graph(PhaseGraph& phase) {
        phase.graph = std::make_unique<jobs::TaskGraph>();
        std::vector<jobs::TaskGraph::TaskId> tasks(entries.size());
        for (const usize i : phase.systems) {
            tasks[i] = phase.graph->add([this, i] { run_system(i); });
        }
        for (const usize i : phase.systems) {
            for (const usize before : entries[i].waits_for) {
                phase.graph->precede(tasks[before], tasks[i]);
            }
        }
    }

    std::vector<Entry> entries;
    std::array<PhaseGraph, phase_count> phases;
    jobs::JobSystem* jobs = nullptr;
};

Schedule::Schedule(World& world) : world_{&world}, impl_{std::make_unique<Impl>()} {}

Schedule::~Schedule() = default;

void Schedule::add_system(std::string_view name, SystemOptions options,
                          std::unique_ptr<detail::System> system) {
    verify(!world_->running_systems(), "add() während run()");
    Impl::Entry entry{
        .name = std::string{name},
        .options = options,
        .system = std::move(system),
        .chunks = {},
        .first_piece = {},
        .commands = {},
        .waits_for = {},
    };
    // after zeigt in die Zeichenkette des Aufrufers; nur hier benutzen.
    entry.options.after = {};
    bool found_after = options.after.empty();
    for (usize i = 0; i < impl_->entries.size(); ++i) {
        const Impl::Entry& earlier = impl_->entries[i];
        if (earlier.options.phase != options.phase) {
            continue;
        }
        const bool after = !options.after.empty() && earlier.name == options.after;
        found_after = found_after || after;
        if (after || conflicts(earlier.system->query(), entry.system->query())) {
            entry.waits_for.push_back(i);
        }
    }
    verify(found_after, "Schedule::add(): after nennt kein früheres System derselben Phase");
    const auto phase = static_cast<usize>(options.phase);
    impl_->phases[phase].systems.push_back(impl_->entries.size());
    impl_->phases[phase].graph.reset();
    impl_->entries.push_back(std::move(entry));
}

void Schedule::run(jobs::JobSystem& jobs, Phase phase) {
    NGINE_PROFILE_ZONE("Schedule");
    verify(!world_->running_systems(), "Schedule::run() läuft schon");
    Impl::PhaseGraph& graph = impl_->phases[static_cast<usize>(phase)];
    if (graph.systems.empty()) {
        return;
    }
    if (graph.graph == nullptr) {
        impl_->build_graph(graph);
    }

    // Erst alle Chunks im aufrufenden Thread einsammeln. Danach rufen die Jobs flecs nicht mehr
    // auf, sondern arbeiten nur auf den Spalten; flecs muss also nicht threadsicher sein.
    for (const usize index : graph.systems) {
        Impl::Entry& entry = impl_->entries[index];
        entry.chunks.clear();
        entry.system->query().collect(entry.chunks);
        if (!entry.system->uses_commands()) {
            continue;
        }
        entry.first_piece.clear();
        usize total = 0;
        for (const detail::Chunk& chunk : entry.chunks) {
            entry.first_piece.push_back(total);
            total += Impl::pieces(entry, chunk.count);
        }
        // Puffer bleiben über Läufe erhalten, damit ihr Speicher wiederverwendet wird.
        while (entry.commands.size() < total) {
            entry.commands.emplace_back(*world_);
        }
    }

    impl_->jobs = &jobs;
    world_->running_systems_ = true;
    graph.graph->run_and_wait(jobs);
    world_->running_systems_ = false;
    impl_->jobs = nullptr;

    // Befehle in fester Reihenfolge: System (Reihenfolge von add()), Chunk, Zeile.
    NGINE_PROFILE_ZONE("Befehlspuffer");
    for (const usize index : graph.systems) {
        for (Commands& commands : impl_->entries[index].commands) {
            if (!commands.empty()) {
                commands.apply();
            }
        }
    }
}

usize Schedule::size() const noexcept {
    return impl_->entries.size();
}

bool Schedule::waits_for(usize later, usize earlier) const noexcept {
    if (later >= impl_->entries.size()) {
        return false;
    }
    const auto& list = impl_->entries[later].waits_for;
    return std::ranges::find(list, earlier) != list.end();
}

} // namespace ngine::world
