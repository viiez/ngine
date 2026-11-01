find_package(Catch2 3 REQUIRED)

add_executable(ngine_tests
    core/log_tests.cpp
    core/types_tests.cpp
    filesystem/filesystem_tests.cpp
    jobs/deque_tests.cpp
    jobs/job_system_tests.cpp
    jobs/task_graph_tests.cpp
    memory/allocator_tests.cpp
    memory/budget_tests.cpp
    profile/profile_tests.cpp
    reflect/reflect_tests.cpp
    serialize/json_tests.cpp
    serialize/reflected_tests.cpp
    rhi/vulkan_requirements_tests.cpp
    rhi/vulkan_triangle_tests.cpp
    world/world_tests.cpp
    world/schedule_tests.cpp
    world/scene_tests.cpp)
target_link_libraries(ngine_tests PRIVATE
    ngine::core ngine::filesystem ngine::memory ngine::jobs ngine::profile ngine::reflect
    ngine::serialize ngine::world ngine::rhi ngine::rhi_vulkan ngine::shaders
    Catch2::Catch2WithMain)
ngine_set_compiler_options(ngine_tests)

include(Catch)
catch_discover_tests(ngine_tests)
