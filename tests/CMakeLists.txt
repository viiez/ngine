find_package(Catch2 3 REQUIRED)
# GLM nur als Vergleich für die eigene Mathematik (ADR 0025).
find_package(glm CONFIG REQUIRED)

add_executable(ngine_tests
    core/log_tests.cpp
    core/types_tests.cpp
    filesystem/filesystem_tests.cpp
    jobs/deque_tests.cpp
    jobs/job_system_tests.cpp
    jobs/task_graph_tests.cpp
    math/math_tests.cpp
    math/math_oracle_tests.cpp
    memory/allocator_tests.cpp
    memory/budget_tests.cpp
    profile/profile_tests.cpp
    reflect/reflect_tests.cpp
    serialize/json_tests.cpp
    serialize/reflected_tests.cpp
    rhi/vulkan_requirements_tests.cpp
    rhi/vulkan_triangle_tests.cpp
    rhi/vulkan_resources_tests.cpp
    render/render_graph_tests.cpp
    render/render_gpu_tests.cpp
    render/renderer_tests.cpp
    render/lighting_tests.cpp
    editor_ui/editor_ui_tests.cpp
    editor_ui/editor_ui_gpu_tests.cpp
    asset/gltf_tests.cpp
    world/world_tests.cpp
    world/schedule_tests.cpp
    world/scene_tests.cpp
    world/transform_tests.cpp
    voxel/voxel_tests.cpp
    script/script_tests.cpp
    net/transport_tests.cpp
    replication/replication_tests.cpp)
target_link_libraries(ngine_tests PRIVATE
    ngine::core ngine::filesystem ngine::math ngine::memory ngine::jobs ngine::profile ngine::reflect
    ngine::serialize ngine::world ngine::asset ngine::voxel ngine::script ngine::net ngine::replication ngine::rhi ngine::rhi_vulkan ngine::render ngine::shaders ngine::editor_ui
    Catch2::Catch2WithMain glm::glm-header-only)
ngine_set_compiler_options(ngine_tests)
set_source_files_properties(math/math_oracle_tests.cpp PROPERTIES
    COMPILE_DEFINITIONS "GLM_FORCE_DEPTH_ZERO_TO_ONE;GLM_FORCE_CTOR_INIT")

# Skripte für die Tests von ngine.script; ohne .NET-SDK werden diese Tests übersprungen.
if(NGINE_WITH_DOTNET)
    ngine_add_dotnet_project(ngine_test_scripts
        PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/script/scripts/Tests.Scripts.csproj"
        SOURCES script/scripts/TestScripts.cs
        DEPENDS ngine_script_managed)
    add_dependencies(ngine_tests ngine_test_scripts)
    target_compile_definitions(ngine_tests PRIVATE NGINE_DOTNET_DIR="${NGINE_DOTNET_DIR}")
endif()

include(Catch)
catch_discover_tests(ngine_tests)
