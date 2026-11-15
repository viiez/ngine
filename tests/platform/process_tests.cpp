#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

import ngine.core;
import ngine.platform;

using namespace ngine;

// CMake liegt auf jeder Plattform vor, auf der die Tests gebaut werden.
TEST_CASE("Processes run without a shell and report output and exit code", "[platform]") {
    const std::vector<std::string> echo{"-E", "echo", "Hallo Welt", "mit \"Anführung\""};
    const auto result = platform::run_process(NGINE_CMAKE_COMMAND, echo);
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 0);
    CHECK(result->output.find("Hallo Welt mit \"Anführung\"") != std::string::npos);

    // Ein Exit-Code ungleich 0 ist ein Ergebnis, kein Fehler; stderr kommt mit.
    const std::vector<std::string> fail{"-E", "cat", "gibt-es-nicht.txt"};
    const auto failed = platform::run_process(NGINE_CMAKE_COMMAND, fail);
    REQUIRE(failed.has_value());
    CHECK(failed->exit_code != 0);
    CHECK(failed->output.find("gibt-es-nicht.txt") != std::string::npos);

    // Arbeitsordner.
    const std::vector<std::string> pwd{"-E", "echo_append", "x"};
    CHECK(platform::run_process(NGINE_CMAKE_COMMAND, pwd, NGINE_TEST_DIR).has_value());

    CHECK_FALSE(platform::run_process("ngine-gibt-es-nicht", {}).has_value());
}
