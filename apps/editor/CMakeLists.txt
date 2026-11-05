# Editor (R2): eigene Anwendung neben der Sandbox, braucht ein Fenster (SDL3).
add_executable(ngine_editor main.cpp)
target_sources(ngine_editor PRIVATE FILE_SET CXX_MODULES FILES input.cppm)
target_link_libraries(ngine_editor PRIVATE
    ngine::core ngine::filesystem ngine::math ngine::profile ngine::world ngine::rhi
    ngine::rhi_vulkan ngine::render ngine::platform ngine::editor_ui ngine_app_common)
ngine_set_compiler_options(ngine_editor)
