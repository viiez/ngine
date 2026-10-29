find_package(SDL3 CONFIG REQUIRED)

add_library(ngine_platform)
add_library(ngine::platform ALIAS ngine_platform)

target_sources(ngine_platform
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            platform.cppm
            window.cppm
    PRIVATE
        window_sdl.cpp)

target_link_libraries(ngine_platform
    PUBLIC ngine::core
    PRIVATE SDL3::SDL3)
ngine_set_compiler_options(ngine_platform)
