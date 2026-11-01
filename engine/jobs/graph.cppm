module;

#include <atomic>
#include <deque>
#include <utility>
#include <vector>

export module ngine.jobs:graph;

import ngine.core;
import :function;
import :system;

export namespace ngine::jobs {

// Aufgaben mit Abhängigkeiten. Eine Aufgabe startet erst, wenn alle ihre Vorgänger fertig sind;
// voneinander unabhängige Aufgaben laufen parallel. Ein Graph wird einmal aufgebaut und kann
// danach beliebig oft ausgeführt werden (z. B. einmal pro Frame).
class TaskGraph {
public:
    using TaskId = u32;

    TaskGraph() = default;
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    template <typename F>
    TaskId add(F&& function) {
        Node& node = nodes_.emplace_back();
        node.function.emplace(std::forward<F>(function));
        validated_ = false;
        return static_cast<TaskId>(nodes_.size() - 1);
    }

    // after startet erst, wenn before fertig ist.
    void precede(TaskId before, TaskId after);

    // Startet alle Aufgaben. counter wird null, wenn alle fertig sind; bis dahin darf der Graph
    // weder verändert noch zerstört werden. Bricht bei Zyklen ab.
    void run(JobSystem& jobs, Counter& counter);
    void run_and_wait(JobSystem& jobs);

    [[nodiscard]] usize size() const noexcept {
        return nodes_.size();
    }

private:
    struct Node {
        JobFunction function;
        std::vector<TaskId> successors;
        u32 predecessors = 0;
        std::atomic<u32> remaining{0};
    };

    void schedule(JobSystem& jobs, Counter& counter, TaskId id);
    void validate() const;

    // std::deque verschiebt seine Elemente beim Wachsen nicht.
    std::deque<Node> nodes_;
    bool validated_ = false;
};

} // namespace ngine::jobs
