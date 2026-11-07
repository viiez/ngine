# Editor (R2): eigene Anwendung neben der Sandbox, braucht ein Fenster (SDL3).
add_executable(ngine_editor main.cpp)
target_sources(ngine_editor PRIVATE FILE_SET CXX_MODULES FILES
    assets.cppm input.cppm inspector_panel.cppm menu.cppm overlay.cppm scene_tree.cppm
    viewport_tools.cppm)
target_link_libraries(ngine_editor PRIVATE
    ngine::core ngine::edit ngine::filesystem ngine::math ngine::profile ngine::reflect
    ngine::world ngine::asset ngine::rhi ngine::rhi_vulkan ngine::render ngine::platform
    ngine::editor_ui ngine::shader_compiler ngine_app_common)
ngine_set_compiler_options(ngine_editor)
ngine_copy_slang_runtime(ngine_editor)
