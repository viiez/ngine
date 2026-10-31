add_library(ngine_rhi)
add_library(ngine::rhi ALIAS ngine_rhi)

target_sources(ngine_rhi
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            rhi.cppm)

target_link_libraries(ngine_rhi PUBLIC ngine::core)
ngine_set_compiler_options(ngine_rhi)

add_subdirectory(vulkan)
