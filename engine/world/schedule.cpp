module;

#include <ngine/profile.hpp>

#include <algorithm>
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
        std::vector<usize> waits_for;
    };

    void run_system(usize index) {
        NGINE_PROFILE_ZONE("System");
        Entry& entry = entries[index];
        for (const detail::Chunk& chunk : entry.chunks) {
            if (entry.options.grain == 0 || chunk.count <= entry.options.grain) {
                entry.system->run_rows(chunk, 0, chunk.count);
                continue;
            }
            jobs->parallel_for(chunk.count, entry.options.grain, [&](usize begin, usize end) {
                entry.system->run_rows(chunk, static_cast<u32>(begin), static_cast<u32>(end));
            });
        }
    }

    void build_graph() {
        graph = std::make_unique<jobs::TaskGraph>();
        std::vector<jobs::TaskGraph::TaskId> tasks;
        for (usize i = 0; i < entries.size(); ++i) {
            tasks.push_back(graph->add([this, i] { run_system(i); }));
        }
        for (usize i = 0; i < entries.size(); ++i) {
            for (const usize before : entries[i].waits_for) {
                graph->precede(tasks[before], tasks[i]);
            }
        }
    }

    std::vector<Entry> entries;
    std::unique_ptr<jobs::TaskGraph> graph;
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
        .waits_for = {},
    };
    for (usize i = 0; i < impl_->entries.size(); ++i) {
        if (conflicts(impl_->entries[i].system->query(), entry.system->query())) {
            entry.waits_for.push_back(i);
        }
    }
    impl_->entries.push_back(std::move(entry));
    impl_->graph.reset();
}

void Schedule::run(jobs::JobSystem& jobs) {
    NGINE_PROFILE_ZONE("Schedule");
    verify(!world_->running_systems(), "Schedule::run() läuft schon");
    if (impl_->graph == nullptr) {
        impl_->build_graph();
    }

    // Erst alle Chunks im aufrufenden Thread einsammeln. Danach rufen die Jobs flecs nicht mehr
    // auf, sondern arbeiten nur auf den Spalten; flecs muss also nicht threadsicher sein.
    for (Impl::Entry& entry : impl_->entries) {
        entry.chunks.clear();
        entry.system->query().collect(entry.chunks);
    }

    impl_->jobs = &jobs;
    world_->running_systems_ = true;
    impl_->graph->run_and_wait(jobs);
    world_->running_systems_ = false;
    impl_->jobs = nullptr;
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
