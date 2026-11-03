add_library(ngine_render)
add_library(ngine::render ALIAS ngine_render)

target_sources(ngine_render
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            render.cppm
            pool.cppm
            graph.cppm
    PRIVATE
        pool.cpp
        graph.cpp)

target_link_libraries(ngine_render PUBLIC ngine::core ngine::rhi PRIVATE ngine::profile)
ngine_set_compiler_options(ngine_render)
