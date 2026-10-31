module;

#include <atomic>
#include <deque>
#include <vector>

module ngine.jobs;

import ngine.core;

namespace ngine::jobs {

void TaskGraph::precede(TaskId before, TaskId after) {
    verify(before < nodes_.size() && after < nodes_.size(), "TaskGraph: unbekannte Aufgabe");
    verify(before != after, "TaskGraph: Aufgabe kann nicht von sich selbst abhängen");
    nodes_[before].successors.push_back(after);
    ++nodes_[after].predecessors;
    validated_ = false;
}

void TaskGraph::validate() const {
    // Kahns Algorithmus: Lassen sich nicht alle Knoten topologisch ordnen, gibt es einen Zyklus.
    std::vector<u32> remaining(nodes_.size());
    std::vector<TaskId> ready;
    for (TaskId id = 0; id < nodes_.size(); ++id) {
        remaining[id] = nodes_[id].predecessors;
        if (remaining[id] == 0) {
            ready.push_back(id);
        }
    }
    usize visited = 0;
    while (!ready.empty()) {
        const TaskId id = ready.back();
        ready.pop_back();
        ++visited;
        for (const TaskId successor : nodes_[id].successors) {
            if (--remaining[successor] == 0) {
                ready.push_back(successor);
            }
        }
    }
    verify(visited == nodes_.size(), "TaskGraph enthält einen Zyklus");
}

void TaskGraph::run(JobSystem& jobs, Counter& counter) {
    if (!validated_) {
        validate();
        validated_ = true;
    }
    for (Node& node : nodes_) {
        node.remaining.store(node.predecessors, std::memory_order_relaxed);
    }
    // Erst alle Zähler setzen, dann starten: Die Jobs laufen sofort los. Die zusätzliche Eins
    // verhindert, dass counter null wird, solange noch nicht alle Wurzeln gestartet sind.
    counter.value_.fetch_add(1, std::memory_order_relaxed);
    for (TaskId id = 0; id < nodes_.size(); ++id) {
        if (nodes_[id].predecessors == 0) {
            schedule(jobs, counter, id);
        }
    }
    counter.value_.fetch_sub(1, std::memory_order_release);
}

void TaskGraph::run_and_wait(JobSystem& jobs) {
    Counter counter;
    run(jobs, counter);
    jobs.wait(counter);
}

void TaskGraph::schedule(JobSystem& jobs, Counter& counter, TaskId id) {
    jobs.run(
        [this, &jobs, &counter, id] {
            Node& node = nodes_[id];
            node.function();
            // Der letzte fertige Vorgänger startet den Nachfolger. acq_rel gibt die Ergebnisse
            // aller Vorgänger an ihn weiter. Der Nachfolger erhöht den Zähler, bevor dieser Job
            // ihn verringert; deshalb wird er nicht zu früh null.
            for (const TaskId successor : node.successors) {
                if (nodes_[successor].remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    schedule(jobs, counter, successor);
                }
            }
        },
        &counter);
}

} // namespace ngine::jobs
