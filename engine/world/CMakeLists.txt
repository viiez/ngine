find_package(flecs CONFIG REQUIRED)

add_library(ngine_world)
add_library(ngine::world ALIAS ngine_world)

target_sources(ngine_world
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            world.cppm
            ecs.cppm
            components.cppm
            transform.cppm
            schedule.cppm
            scene.cppm
    PRIVATE
        world.cpp
        schedule.cpp
        scene.cpp
        transform.cpp)

# flecs steckt nur in world.cpp (ADR 0009). Alle anderen sehen World, Entity und Query.
target_link_libraries(ngine_world
    PUBLIC ngine::core ngine::math ngine::reflect ngine::jobs
    PRIVATE ngine::serialize ngine::filesystem ngine::profile
            $<IF:$<TARGET_EXISTS:flecs::flecs>,flecs::flecs,flecs::flecs_static>)
ngine_set_compiler_options(ngine_world)
