add_library(ngine_filesystem)
add_library(ngine::filesystem ALIAS ngine_filesystem)

target_sources(ngine_filesystem
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            filesystem.cppm
    PRIVATE
        FILE_SET internal TYPE CXX_MODULES
        FILES
            platform.cppm
    PRIVATE
        filesystem.cpp)

# Plattformcode nur in den Adaptern: je eine Datei pro Betriebssystem.
if(WIN32)
    target_sources(ngine_filesystem PRIVATE user_dir_windows.cpp)
    target_link_libraries(ngine_filesystem PRIVATE shell32 ole32)
else()
    target_sources(ngine_filesystem PRIVATE user_dir_linux.cpp)
endif()

target_link_libraries(ngine_filesystem PUBLIC ngine::core)
ngine_set_compiler_options(ngine_filesystem)
