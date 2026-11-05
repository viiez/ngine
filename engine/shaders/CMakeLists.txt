add_library(ngine_shaders)
add_library(ngine::shaders ALIAS ngine_shaders)

target_sources(ngine_shaders
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            shaders.cppm)

ngine_compile_shader(ngine_shaders SOURCE triangle.slang ENTRY vertex_main STAGE vertex NAME triangle_vertex)
ngine_compile_shader(ngine_shaders SOURCE triangle.slang ENTRY fragment_main STAGE fragment NAME triangle_fragment)
ngine_compile_shader(ngine_shaders SOURCE voxel.slang ENTRY vertex_main STAGE vertex NAME voxel_vertex)
ngine_compile_shader(ngine_shaders SOURCE voxel.slang ENTRY fragment_main STAGE fragment NAME voxel_fragment)
ngine_compile_shader(ngine_shaders SOURCE fullscreen.slang ENTRY vertex_main STAGE vertex NAME fullscreen_vertex DEPENDS bindless.slang)
ngine_compile_shader(ngine_shaders SOURCE fullscreen.slang ENTRY fragment_main STAGE fragment NAME fullscreen_fragment DEPENDS bindless.slang)
ngine_compile_shader(ngine_shaders SOURCE pbr.slang ENTRY vertex_main STAGE vertex NAME pbr_vertex DEPENDS bindless.slang scene.slang)
ngine_compile_shader(ngine_shaders SOURCE pbr.slang ENTRY fragment_main STAGE fragment NAME pbr_fragment DEPENDS bindless.slang scene.slang)
ngine_compile_shader(ngine_shaders SOURCE shadow.slang ENTRY vertex_main STAGE vertex NAME shadow_vertex DEPENDS bindless.slang scene.slang)
ngine_compile_shader(ngine_shaders SOURCE shadow.slang ENTRY fragment_main STAGE fragment NAME shadow_fragment DEPENDS bindless.slang scene.slang)
ngine_compile_shader(ngine_shaders SOURCE debug.slang ENTRY vertex_main STAGE vertex NAME debug_vertex DEPENDS bindless.slang scene.slang)
ngine_compile_shader(ngine_shaders SOURCE debug.slang ENTRY fragment_main STAGE fragment NAME debug_fragment DEPENDS bindless.slang scene.slang)
ngine_compile_shader(ngine_shaders SOURCE tonemap.slang ENTRY fragment_main STAGE fragment NAME tonemap_fragment DEPENDS bindless.slang)
ngine_compile_shader(ngine_shaders SOURCE ui.slang ENTRY vertex_main STAGE vertex NAME ui_vertex DEPENDS bindless.slang)
ngine_compile_shader(ngine_shaders SOURCE ui.slang ENTRY fragment_main STAGE fragment NAME ui_fragment DEPENDS bindless.slang)
ngine_compile_shader(ngine_shaders SOURCE gradient.slang ENTRY compute_main STAGE compute NAME gradient_compute DEPENDS bindless.slang)

target_link_libraries(ngine_shaders PUBLIC ngine::core ngine::rhi)
ngine_set_compiler_options(ngine_shaders)
