add_executable(ngine_sandbox main.cpp)
target_sources(ngine_sandbox PRIVATE FILE_SET CXX_MODULES FILES
    frame_stats.cppm jobs_demo.cppm scene_demo.cppm voxel_demo.cppm model_demo.cppm script_demo.cppm
    net_demo.cppm)
target_link_libraries(ngine_sandbox PRIVATE
    ngine::core ngine::filesystem ngine::math ngine::memory ngine::jobs ngine::profile ngine::reflect
    ngine::world ngine::asset ngine::voxel ngine::script ngine::net ngine::replication ngine::rhi ngine::rhi_vulkan ngine::render ngine::shaders ngine::input ngine::platform ngine_app_common)
if(NGINE_WITH_WINDOW)
    target_compile_definitions(ngine_sandbox PRIVATE NGINE_WITH_WINDOW=1)
else()
    target_compile_definitions(ngine_sandbox PRIVATE NGINE_WITH_WINDOW=0)
endif()
ngine_set_compiler_options(ngine_sandbox)

# Skripte für --script-demo (D2).
if(NGINE_WITH_DOTNET)
    ngine_add_dotnet_project(ngine_sandbox_scripts
        PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Sandbox.Scripts.csproj"
        SOURCES scripts/Orbit.cs
        DEPENDS ngine_script_managed)
    ngine_copy_dotnet_assemblies(ngine_sandbox DEPENDS ngine_sandbox_scripts)
    target_compile_definitions(ngine_sandbox PRIVATE NGINE_WITH_DOTNET=1)
else()
    target_compile_definitions(ngine_sandbox PRIVATE NGINE_WITH_DOTNET=0)
endif()
