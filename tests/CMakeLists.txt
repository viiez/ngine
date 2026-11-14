find_package(Catch2 3 REQUIRED)
# GLM nur als Vergleich für die eigene Mathematik (ADR 0025).
find_package(glm CONFIG REQUIRED)

add_executable(ngine_tests
    core/log_tests.cpp
    core/events_tests.cpp
    core/types_tests.cpp
    filesystem/filesystem_tests.cpp
    input/input_tests.cpp
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
    render/geometry_tests.cpp
    render/culling_tests.cpp
    editor_ui/editor_ui_tests.cpp
    editor_ui/editor_ui_gpu_tests.cpp
    asset/gltf_tests.cpp
    asset/resources_tests.cpp
    asset/cooked_tests.cpp
    cook/cook_tests.cpp
    meshlet/meshlet_tests.cpp
    world/world_tests.cpp
    world/schedule_tests.cpp
    world/commands_tests.cpp
    world/simulation_tests.cpp
    world/scene_tests.cpp
    world/transform_tests.cpp
    world/prefab_tests.cpp
    world/terrain_tests.cpp
    physics/physics_tests.cpp
    edit/edit_tests.cpp
    edit/gizmo_play_tests.cpp
    edit/prefab_edit_tests.cpp
    edit/terrain_edit_tests.cpp
    voxel/voxel_tests.cpp
    voxel/chunk_tests.cpp
    voxel/generator_tests.cpp
    voxel/lod_tests.cpp
    voxel/delta_tests.cpp
    script/script_tests.cpp
    net/transport_tests.cpp
    replication/replication_tests.cpp
    shader_compiler/shader_compiler_tests.cpp)
target_link_libraries(ngine_tests PRIVATE
    ngine::core ngine::filesystem ngine::math ngine::memory ngine::jobs ngine::profile ngine::reflect
    ngine::serialize ngine::world ngine::asset ngine::meshlet ngine::voxel ngine::script ngine::net ngine::replication ngine::rhi ngine::rhi_vulkan ngine::render ngine::shaders ngine::editor_ui ngine::edit ngine::shader_compiler ngine::cook ngine::input ngine::platform ngine::physics
    Catch2::Catch2WithMain glm::glm-header-only)
ngine_set_compiler_options(ngine_tests)
# Quellen der Renderer-Shader für den Test des Hot Reload (R2.4).
set_source_files_properties(shader_compiler/shader_compiler_tests.cpp PROPERTIES
    COMPILE_DEFINITIONS "NGINE_SHADER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}/engine/shaders\"")
ngine_copy_slang_runtime(ngine_tests)
# Beispiel-Assets für den Test des Ladens über Asset-IDs.
set_source_files_properties(render/renderer_tests.cpp cook/cook_tests.cpp input/input_tests.cpp PROPERTIES
    COMPILE_DEFINITIONS "NGINE_ASSET_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}/assets\"")
set_source_files_properties(math/math_oracle_tests.cpp PROPERTIES
    COMPILE_DEFINITIONS "GLM_FORCE_DEPTH_ZERO_TO_ONE;GLM_FORCE_CTOR_INIT")

# Skripte für die Tests von ngine.script; ohne .NET-SDK werden diese Tests übersprungen.
if(NGINE_WITH_DOTNET)
    ngine_add_dotnet_project(ngine_test_scripts
        PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/script/scripts/Tests.Scripts.csproj"
        SOURCES script/scripts/TestScripts.cs
        DEPENDS ngine_script_managed)
    ngine_copy_dotnet_assemblies(ngine_tests DEPENDS ngine_test_scripts)
    target_compile_definitions(ngine_tests PRIVATE NGINE_WITH_DOTNET=1)
endif()

include(Catch)
catch_discover_tests(ngine_tests)
