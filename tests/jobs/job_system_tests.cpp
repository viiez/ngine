#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

import ngine.core;
import ngine.jobs;

using namespace ngine;

TEST_CASE("Jobs run and the counter reaches zero", "[jobs]") {
    jobs::JobSystem system{{.worker_count = 3}};
    std::atomic<int> sum{0};
    jobs::Counter counter;
    for (int i = 1; i <= 1000; ++i) {
        system.run([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); }, &counter);
    }
    system.wait(counter);
    CHECK(counter.done());
    CHECK(sum.load() == 500'500);
}

TEST_CASE("Jobs run without worker threads while waiting", "[jobs]") {
    jobs::JobSystem system{{.worker_count = 0}};
    CHECK(system.worker_count() == 0);
    int sum = 0;
    jobs::Counter counter;
    for (int i = 0; i < 10; ++i) {
        system.run([&sum] { ++sum; }, &counter);
    }
    system.wait(counter);
    CHECK(sum == 10);
}

TEST_CASE("Jobs really run in parallel", "[jobs]") {
    jobs::JobSystem system{{.worker_count = 2}};
    // Beide Jobs warten aufeinander: Das gelingt nur, wenn sie gleichzeitig laufen.
    std::atomic<int> arrived{0};
    std::atomic<bool> timed_out{false};
    auto rendezvous = [&] {
        arrived.fetch_add(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (arrived.load() < 2) {
            if (std::chrono::steady_clock::now() > deadline) {
                timed_out = true;
                return;
            }
            std::this_thread::yield();
        }
    };
    jobs::Counter counter;
    system.run(rendezvous, &counter);
    system.run(rendezvous, &counter);
    system.wait(counter);
    CHECK_FALSE(timed_out.load());
}

TEST_CASE("Jobs are spread over several threads", "[jobs]") {
    jobs::JobSystem system{{.worker_count = 3}};
    std::mutex mutex;
    std::set<std::thread::id> threads;
    jobs::Counter counter;
    for (int i = 0; i < 64; ++i) {
        system.run(
            [&] {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                const std::scoped_lock lock{mutex};
                threads.insert(std::this_thread::get_id());
            },
            &counter);
    }
    system.wait(counter);
    CHECK(threads.size() > 1);
}

TEST_CASE("Jobs can spawn and wait for nested jobs", "[jobs]") {
    jobs::JobSystem system{{.worker_count = 3}};
    std::atomic<int> leaves{0};
    jobs::Counter outer;
    for (int i = 0; i < 16; ++i) {
        system.run(
            [&] {
                jobs::Counter inner;
                for (int j = 0; j < 16; ++j) {
                    system.run([&] { leaves.fetch_add(1, std::memory_order_relaxed); }, &inner);
                }
                system.wait(inner);
            },
            &outer);
    }
    system.wait(outer);
    CHECK(leaves.load() == 256);
}

TEST_CASE("Jobs can be submitted from foreign threads", "[jobs]") {
    jobs::JobSystem system{{.worker_count = 2}};
    std::atomic<int> count{0};
    {
        std::vector<std::jthread> producers;
        for (int t = 0; t < 4; ++t) {
            producers.emplace_back([&] {
                jobs::Counter counter;
                for (int i = 0; i < 500; ++i) {
                    system.run([&] { count.fetch_add(1, std::memory_order_relaxed); }, &counter);
                }
                system.wait(counter);
            });
        }
    }
    CHECK(count.load() == 2000);
}

TEST_CASE("parallel_for covers the range exactly once", "[jobs]") {
    jobs::JobSystem system{{.worker_count = 3}};
    std::vector<int> hits(10'007, 0);
    system.parallel_for(hits.size(), 64, [&](usize begin, usize end) {
        CHECK(end - begin <= 64);
        for (usize i = begin; i < end; ++i) {
            ++hits[i];
        }
    });
    CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));

    std::atomic<int> calls{0};
    system.parallel_for(0, 8, [&](usize, usize) { ++calls; });
    CHECK(calls.load() == 0);
}

TEST_CASE("Destroying the job system finishes open jobs", "[jobs]") {
    std::atomic<int> count{0};
    {
        jobs::JobSystem system{{.worker_count = 2}};
        for (int i = 0; i < 100; ++i) {
            system.run([&] { count.fetch_add(1, std::memory_order_relaxed); });
        }
    }
    CHECK(count.load() == 100);
}

TEST_CASE("Job system survives a stress test", "[jobs][stress]") {
    jobs::JobSystem system;
    std::atomic<u64> sum{0};
    for (int round = 0; round < 20; ++round) {
        jobs::Counter counter;
        for (int i = 0; i < 2000; ++i) {
            system.run([&sum, i] { sum.fetch_add(static_cast<u64>(i), std::memory_order_relaxed); },
                       &counter);
        }
        system.wait(counter);
    }
    CHECK(sum.load() == 20ull * (1999ull * 2000ull / 2));
}
