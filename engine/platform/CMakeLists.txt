add_library(ngine_platform)
add_library(ngine::platform ALIAS ngine_platform)

target_sources(ngine_platform
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            platform.cppm
            window.cppm)

target_link_libraries(ngine_platform PUBLIC ngine::core)

# Ohne Fenster gibt es nur die Typen der Ereignisse (R4.1); create() meldet dann einen Fehler.
if(NGINE_WITH_WINDOW)
    find_package(SDL3 CONFIG REQUIRED)
    target_sources(ngine_platform PRIVATE window_sdl.cpp)
    target_link_libraries(ngine_platform PRIVATE SDL3::SDL3)
else()
    target_sources(ngine_platform PRIVATE window_null.cpp)
endif()
ngine_set_compiler_options(ngine_platform)
