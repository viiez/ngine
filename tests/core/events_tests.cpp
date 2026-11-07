#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

import ngine.core;

using namespace ngine;

namespace {

struct Moved {
    i32 x = 0;
};

struct Named {
    std::string name;
};

} // namespace

TEST_CASE("Events reach the subscribers of their type in order", "[core][events]") {
    events::EventBus bus;
    std::vector<std::string> calls;
    auto first =
        bus.subscribe<Moved>([&](const Moved& e) { calls.push_back("a" + std::to_string(e.x)); });
    auto second =
        bus.subscribe<Moved>([&](const Moved& e) { calls.push_back("b" + std::to_string(e.x)); });
    auto other = bus.subscribe<Named>([&](const Named& e) { calls.push_back(e.name); });
    CHECK(bus.subscribers<Moved>() == 2);

    bus.publish(Moved{1});
    bus.publish(Named{"n"});
    CHECK(calls == std::vector<std::string>{"a1", "b1", "n"});
}

TEST_CASE("Subscriptions unsubscribe when destroyed or reset", "[core][events]") {
    events::EventBus bus;
    i32 count = 0;
    {
        auto subscription = bus.subscribe<Moved>([&](const Moved&) { ++count; });
        bus.publish(Moved{});
        CHECK(subscription.active());
    }
    bus.publish(Moved{});
    CHECK(count == 1);
    CHECK(bus.subscribers<Moved>() == 0);

    auto subscription = bus.subscribe<Moved>([&](const Moved&) { ++count; });
    events::Subscription moved = std::move(subscription);
    CHECK_FALSE(subscription.active());
    moved.reset();
    bus.publish(Moved{});
    CHECK(count == 1);
}

TEST_CASE("Queued events are delivered on dispatch, including follow-ups", "[core][events]") {
    events::EventBus bus;
    std::vector<i32> seen;
    auto subscription = bus.subscribe<Moved>([&](const Moved& e) {
        seen.push_back(e.x);
        if (e.x < 3) {
            bus.enqueue(Moved{e.x + 1});
        }
    });
    bus.enqueue(Moved{1});
    CHECK(seen.empty());
    CHECK(bus.pending() == 1);
    CHECK(bus.dispatch() == 3);
    CHECK(seen == std::vector<i32>{1, 2, 3});
    CHECK(bus.pending() == 0);
}

TEST_CASE("Handlers may unsubscribe and subscribe while events are delivered", "[core][events]") {
    events::EventBus bus;
    i32 late_calls = 0;
    i32 second_calls = 0;
    std::optional<events::Subscription> late;
    events::Subscription second;
    auto first = bus.subscribe<Moved>([&](const Moved&) {
        second.reset();
        if (!late) {
            late = bus.subscribe<Moved>([&](const Moved&) { ++late_calls; });
        }
    });
    second = bus.subscribe<Moved>([&](const Moved&) { ++second_calls; });
    bus.publish(Moved{});
    CHECK(second_calls == 0);
    CHECK(late_calls == 0);
    bus.publish(Moved{});
    CHECK(late_calls == 1);
}

TEST_CASE("Subscriptions may outlive their bus", "[core][events]") {
    events::Subscription subscription;
    {
        events::EventBus bus;
        subscription = bus.subscribe<Moved>([](const Moved&) {});
    }
    CHECK_FALSE(subscription.active());
    subscription.reset();
}
