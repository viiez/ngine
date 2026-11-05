find_package(imgui CONFIG REQUIRED)

add_library(ngine_editor_ui)
add_library(ngine::editor_ui ALIAS ngine_editor_ui)

target_sources(ngine_editor_ui
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            editor_ui.cppm
            draw.cppm
            context.cppm
            widgets.cppm
            pass.cppm
            console.cppm
            viewport.cppm
    PRIVATE
        context.cpp
        widgets.cpp
        pass.cpp
        console.cpp)

# Dear ImGui steckt nur in context.cpp und widgets.cpp (ADR 0029); der Pass kennt nur die RHI. Die
# Bibliothek braucht weder Fenster noch GPU, damit Tests ohne beides laufen.
target_link_libraries(ngine_editor_ui
    PUBLIC ngine::core ngine::rhi ngine::render
    PRIVATE ngine::profile ngine::shaders imgui::imgui)
ngine_set_compiler_options(ngine_editor_ui)
