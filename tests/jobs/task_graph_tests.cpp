#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <vector>

import ngine.core;
import ngine.jobs;

using namespace ngine;

TEST_CASE("TaskGraph respects dependencies", "[jobs][graph]") {
    jobs::JobSystem system{{.worker_count = 3}};
    jobs::TaskGraph graph;
    std::atomic<int> step{0};
    int a_seen = -1;
    int b_seen = -1;
    int c_seen = -1;
    int d_seen = -1;

    // Raute: a vor b und c, beide vor d.
    const auto a = graph.add([&] { a_seen = step.fetch_add(1); });
    const auto b = graph.add([&] { b_seen = step.fetch_add(1); });
    const auto c = graph.add([&] { c_seen = step.fetch_add(1); });
    const auto d = graph.add([&] { d_seen = step.fetch_add(1); });
    graph.precede(a, b);
    graph.precede(a, c);
    graph.precede(b, d);
    graph.precede(c, d);

    graph.run_and_wait(system);
    CHECK(a_seen == 0);
    CHECK(b_seen > a_seen);
    CHECK(c_seen > a_seen);
    CHECK(d_seen == 3);
}

TEST_CASE("TaskGraph passes results along a chain and can run again", "[jobs][graph]") {
    jobs::JobSystem system{{.worker_count = 2}};
    jobs::TaskGraph graph;
    std::vector<int> values(50, 0);
    jobs::TaskGraph::TaskId previous = 0;
    for (usize i = 0; i < values.size(); ++i) {
        const auto id = graph.add([&values, i] { values[i] = i == 0 ? 1 : values[i - 1] + 1; });
        if (i > 0) {
            graph.precede(previous, id);
        }
        previous = id;
    }

    for (int round = 0; round < 3; ++round) {
        std::fill(values.begin(), values.end(), 0);
        graph.run_and_wait(system);
        CHECK(values.back() == 50);
    }
}

TEST_CASE("TaskGraph runs independent tasks in a wide graph", "[jobs][graph]") {
    jobs::JobSystem system{{.worker_count = 3}};
    jobs::TaskGraph graph;
    std::atomic<int> count{0};
    std::atomic<int> final_seen{0};
    const auto last = graph.add([&] { final_seen = count.load(); });
    for (int i = 0; i < 200; ++i) {
        const auto id = graph.add([&] { count.fetch_add(1); });
        graph.precede(id, last);
    }
    jobs::Counter counter;
    graph.run(system, counter);
    system.wait(counter);
    CHECK(final_seen.load() == 200);
}
