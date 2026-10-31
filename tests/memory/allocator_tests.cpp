#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory_resource>
#include <set>
#include <thread>
#include <vector>

import ngine.core;
import ngine.memory;

using namespace ngine;
using memory::Category;

namespace {

bool is_aligned(const void* pointer, usize alignment) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

} // namespace

TEST_CASE("align_up rounds to the next multiple", "[memory][align]") {
    STATIC_REQUIRE(memory::align_up(0, 16) == 0);
    STATIC_REQUIRE(memory::align_up(1, 16) == 16);
    STATIC_REQUIRE(memory::align_up(16, 16) == 16);
    STATIC_REQUIRE(memory::align_up(17, 8) == 24);
    STATIC_REQUIRE(memory::is_power_of_two(64));
    STATIC_REQUIRE_FALSE(memory::is_power_of_two(48));
}

TEST_CASE("LinearAllocator hands out aligned memory in order", "[memory][linear]") {
    memory::LinearAllocator linear{1024, Category::General};
    void* a = linear.allocate(3, 1);
    void* b = linear.allocate(8, 64);
    CHECK(is_aligned(b, 64));
    CHECK(static_cast<std::byte*>(b) > static_cast<std::byte*>(a));
    CHECK(linear.used() >= 11);

    const usize marker = linear.marker();
    [[maybe_unused]] void* c = linear.allocate(100, 8);
    linear.rewind(marker);
    CHECK(linear.used() == marker);
    CHECK(linear.allocate(100, 8) == c);

    linear.reset();
    CHECK(linear.used() == 0);
}

TEST_CASE("LinearAllocator works with pmr containers", "[memory][linear]") {
    memory::LinearAllocator linear{4096, Category::General};
    std::pmr::vector<u64> values{&linear};
    for (u64 i = 0; i < 100; ++i) {
        values.push_back(i);
    }
    CHECK(values[99] == 99);
    CHECK(linear.used() >= 100 * sizeof(u64));
}

TEST_CASE("LinearAllocator books its capacity", "[memory][linear]") {
    const auto before = memory::stats(Category::World);
    {
        memory::LinearAllocator linear{2048, Category::World};
        CHECK(memory::stats(Category::World).current == before.current + 2048);
    }
    CHECK(memory::stats(Category::World).current == before.current);
}

TEST_CASE("FrameAllocator keeps a frame alive until it comes around again", "[memory][frame]") {
    memory::FrameAllocator frames{256, 2, Category::Rendering};
    auto* first = static_cast<u32*>(frames.allocate(sizeof(u32), alignof(u32)));
    *first = 42;
    CHECK(frames.frame_index() == 0);

    frames.next_frame();
    CHECK(frames.frame_index() == 1);
    CHECK(frames.used() == 0);
    auto* second = static_cast<u32*>(frames.allocate(sizeof(u32), alignof(u32)));
    *second = 7;
    // Der vorige Frame ist noch nicht überschrieben.
    CHECK(*first == 42);
    CHECK(second != first);

    frames.next_frame();
    CHECK(frames.frame_index() == 0);
    CHECK(frames.allocate(sizeof(u32), alignof(u32)) == first);
    CHECK(frames.peak() >= sizeof(u32));
}

TEST_CASE("FrameAllocator allocates concurrently without overlap", "[memory][frame]") {
    constexpr int threads = 4;
    constexpr int per_thread = 1000;
    memory::FrameAllocator frames{threads * per_thread * 32, 2, Category::Jobs};

    std::vector<std::vector<void*>> results(threads);
    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                for (int i = 0; i < per_thread; ++i) {
                    void* p = frames.allocate(24, 8);
                    *static_cast<int*>(p) = t;
                    results[static_cast<usize>(t)].push_back(p);
                }
            });
        }
    }

    std::set<void*> unique;
    for (int t = 0; t < threads; ++t) {
        for (void* p : results[static_cast<usize>(t)]) {
            CHECK(*static_cast<int*>(p) == t);
            CHECK(is_aligned(p, 8));
            unique.insert(p);
        }
    }
    CHECK(unique.size() == threads * per_thread);
}

TEST_CASE("PoolAllocator reuses freed blocks", "[memory][pool]") {
    memory::PoolAllocator pool{24, 8, 4, Category::General};
    CHECK(pool.block_size() >= 24);

    std::vector<void*> blocks;
    for (int i = 0; i < 10; ++i) {
        blocks.push_back(pool.allocate_block());
        CHECK(is_aligned(blocks.back(), 8));
    }
    CHECK(pool.blocks_in_use() == 10);
    CHECK(pool.capacity() == 12);

    void* freed = blocks[3];
    pool.deallocate_block(freed);
    CHECK(pool.allocate_block() == freed);

    for (void* block : blocks) {
        pool.deallocate_block(block);
    }
    CHECK(pool.blocks_in_use() == 0);
    CHECK(pool.capacity() == 12);
}

TEST_CASE("ObjectPool constructs and destroys objects", "[memory][pool]") {
    struct Counted {
        explicit Counted(int& counter) : counter_{counter} {
            ++counter_;
        }
        ~Counted() {
            --counter_;
        }
        Counted(const Counted&) = delete;
        Counted& operator=(const Counted&) = delete;
        int& counter_;
        alignas(32) u8 payload[40]{};
    };

    int alive = 0;
    memory::ObjectPool<Counted> pool{8};
    std::vector<Counted*> objects;
    for (int i = 0; i < 20; ++i) {
        objects.push_back(pool.create(alive));
        CHECK(is_aligned(objects.back(), 32));
    }
    CHECK(alive == 20);
    CHECK(pool.size() == 20);
    for (Counted* object : objects) {
        pool.destroy(object);
    }
    CHECK(alive == 0);
    CHECK(pool.size() == 0);
}
