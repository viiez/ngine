#include <catch2/catch_test_macros.hpp>

#include <memory_resource>
#include <thread>
#include <vector>

import ngine.core;
import ngine.memory;

using namespace ngine;
using memory::Category;

TEST_CASE("Budget counts allocations and peak per category", "[memory][budget]") {
    const auto before = memory::stats(Category::Audio);
    memory::record_allocation(Category::Audio, 100);
    memory::record_allocation(Category::Audio, 50);
    memory::record_deallocation(Category::Audio, 100);

    const auto after = memory::stats(Category::Audio);
    CHECK(after.current == before.current + 50);
    CHECK(after.peak >= before.current + 150);
    CHECK(after.allocations == before.allocations + 2);
    memory::record_deallocation(Category::Audio, 50);
}

TEST_CASE("Budget is reported per category", "[memory][budget]") {
    memory::set_budget(Category::Network, 1024);
    CHECK(memory::stats(Category::Network).budget == 1024);
    // Überschreiten warnt nur, bricht aber nicht ab.
    memory::record_allocation(Category::Network, 2048);
    CHECK(memory::stats(Category::Network).current >= 2048);
    memory::record_deallocation(Category::Network, 2048);
    memory::set_budget(Category::Network, 0);
}

TEST_CASE("Budget counters are thread safe", "[memory][budget]") {
    const auto before = memory::stats(Category::Scripting);
    constexpr int threads = 4;
    constexpr int iterations = 10'000;
    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([] {
                for (int i = 0; i < iterations; ++i) {
                    memory::record_allocation(Category::Scripting, 16);
                    memory::record_deallocation(Category::Scripting, 16);
                }
            });
        }
    }
    const auto after = memory::stats(Category::Scripting);
    CHECK(after.current == before.current);
    CHECK(after.allocations == before.allocations + threads * iterations);
}

TEST_CASE("TrackingResource books standard containers", "[memory][budget]") {
    memory::TrackingResource resource{Category::Tools};
    const auto before = memory::stats(Category::Tools);
    {
        std::pmr::vector<u32> values{&resource};
        values.resize(256);
        CHECK(memory::stats(Category::Tools).current >= before.current + 256 * sizeof(u32));
    }
    CHECK(memory::stats(Category::Tools).current == before.current);
}
