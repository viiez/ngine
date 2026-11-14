find_package(Jolt CONFIG REQUIRED)
# Jolt gibt Schalter wie -pthread, -msse2 und -mfpmath=sse an Nutzer weiter. Moduldateien müssen
# mit denselben Schaltern übersetzt sein wie ihre Nutzer, sonst lädt Clang sie nicht; auf x86-64
# sind SSE2 und SSE-Gleitkomma ohnehin Standard, Threads bindet die Engine selbst ein. Die
# Definitionen (JPH_DOUBLE_PRECISION) bleiben.
set_property(TARGET Jolt::Jolt PROPERTY INTERFACE_COMPILE_OPTIONS "")

add_library(ngine_physics)
add_library(ngine::physics ALIAS ngine_physics)

target_sources(ngine_physics
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            physics.cppm
            components.cppm
            physics_world.cppm
    PRIVATE
        components.cpp
        jolt_world.cpp)

# Jolt steckt nur in jolt_*.cpp (ADR 0045). Der Port in cmake/vcpkg-ports baut mit doppelter
# Genauigkeit und ohne Befehlssatz-Schalter, die an Nutzer weitergingen.
target_link_libraries(ngine_physics
    PUBLIC ngine::core ngine::math ngine::reflect ngine::jobs ngine::world
    PRIVATE ngine::profile Jolt::Jolt)
ngine_set_compiler_options(ngine_physics)
