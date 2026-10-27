#include <catch2/catch_test_macros.hpp>

import ngine.core;

TEST_CASE("Zahlentypen haben die erwartete Breite", "[core][types]") {
    STATIC_REQUIRE(sizeof(ngine::u8) == 1);
    STATIC_REQUIRE(sizeof(ngine::u16) == 2);
    STATIC_REQUIRE(sizeof(ngine::u32) == 4);
    STATIC_REQUIRE(sizeof(ngine::u64) == 8);
    STATIC_REQUIRE(sizeof(ngine::i8) == 1);
    STATIC_REQUIRE(sizeof(ngine::i16) == 2);
    STATIC_REQUIRE(sizeof(ngine::i32) == 4);
    STATIC_REQUIRE(sizeof(ngine::i64) == 8);
    STATIC_REQUIRE(sizeof(ngine::f32) == 4);
    STATIC_REQUIRE(sizeof(ngine::f64) == 8);
}

TEST_CASE("Engine-Version kommt aus CMake", "[core][version]") {
    STATIC_REQUIRE(ngine::engine_name == "NGine");
    STATIC_REQUIRE(ngine::engine_version.major == 0);
    STATIC_REQUIRE(ngine::engine_version.minor >= 1);
}
