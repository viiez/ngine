add_library(ngine_reflect)
add_library(ngine::reflect ALIAS ngine_reflect)

target_sources(ngine_reflect
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            reflect.cppm
            type.cppm
            registry.cppm
    PRIVATE
        registry.cpp)

target_link_libraries(ngine_reflect PUBLIC ngine::core)
ngine_set_compiler_options(ngine_reflect)
