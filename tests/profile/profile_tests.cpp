#include <catch2/catch_test_macros.hpp>

#include <ngine/profile.hpp>

import ngine.core;
import ngine.profile;

namespace {

int profiled_work(int value) {
    NGINE_PROFILE_ZONE("profiled_work");
    {
        NGINE_PROFILE_ZONE_COLOR("inner", 0x3080ff);
        value *= 2;
    }
    return value + 1;
}

} // namespace

TEST_CASE("Profiling zones and markers can be used with and without Tracy", "[profile]") {
    ngine::profile::set_thread_name("Test");
    CHECK(profiled_work(20) == 41);
    ngine::profile::plot("Testwert", 1.5);
    ngine::profile::message("Nachricht aus dem Test");
    ngine::profile::frame_mark();
}
