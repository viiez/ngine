add_library(ngine_shaders)
add_library(ngine::shaders ALIAS ngine_shaders)

target_sources(ngine_shaders
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            shaders.cppm)

ngine_compile_shader(ngine_shaders SOURCE triangle.slang ENTRY vertex_main STAGE vertex NAME triangle_vertex)
ngine_compile_shader(ngine_shaders SOURCE triangle.slang ENTRY fragment_main STAGE fragment NAME triangle_fragment)

target_link_libraries(ngine_shaders PUBLIC ngine::core ngine::rhi)
ngine_set_compiler_options(ngine_shaders)
