#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

import ngine.core;
import ngine.jobs;

using namespace ngine;

TEST_CASE("Deque pops LIFO and steals FIFO", "[jobs][deque]") {
    jobs::WorkStealingDeque<int*> deque{4};
    int values[3] = {1, 2, 3};
    for (int& value : values) {
        deque.push(&value);
    }
    CHECK(deque.size() == 3);
    CHECK(deque.steal() == &values[0]);
    CHECK(deque.pop() == &values[2]);
    CHECK(deque.pop() == &values[1]);
    CHECK(deque.pop() == nullptr);
    CHECK(deque.steal() == nullptr);
}

TEST_CASE("Deque grows beyond its initial capacity", "[jobs][deque]") {
    jobs::WorkStealingDeque<int*> deque{2};
    std::vector<int> values(100);
    for (int& value : values) {
        deque.push(&value);
    }
    CHECK(deque.size() == 100);
    for (usize i = values.size(); i > 0; --i) {
        CHECK(deque.pop() == &values[i - 1]);
    }
}

TEST_CASE("Deque hands out every item exactly once under contention", "[jobs][deque]") {
    constexpr int items = 20'000;
    constexpr int thieves = 3;
    jobs::WorkStealingDeque<int*> deque{8};
    std::vector<int> values(items, 0);
    std::vector<std::atomic<int>> taken(items);
    std::atomic<int> total{0};
    std::atomic<bool> done{false};

    auto take = [&](int* item) {
        taken[static_cast<usize>(item - values.data())].fetch_add(1, std::memory_order_relaxed);
        total.fetch_add(1, std::memory_order_relaxed);
    };

    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < thieves; ++t) {
            threads.emplace_back([&] {
                while (!done.load(std::memory_order_acquire) || deque.size() > 0) {
                    if (int* item = deque.steal()) {
                        take(item);
                    }
                }
            });
        }
        // Der Besitzer schiebt und holt abwechselnd, damit pop() und steal() sich oft kreuzen.
        for (int i = 0; i < items; ++i) {
            deque.push(&values[static_cast<usize>(i)]);
            if (i % 3 == 0) {
                if (int* item = deque.pop()) {
                    take(item);
                }
            }
        }
        while (int* item = deque.pop()) {
            take(item);
        }
        done.store(true, std::memory_order_release);
    }

    CHECK(total.load() == items);
    for (const auto& count : taken) {
        REQUIRE(count.load() == 1);
    }
}
