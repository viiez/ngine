configure_file(version.cppm.in "${CMAKE_CURRENT_BINARY_DIR}/version.cppm" @ONLY)

add_library(ngine_core)
add_library(ngine::core ALIAS ngine_core)

target_sources(ngine_core
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}"
        FILES
            core.cppm
            types.cppm
            check.cppm
            log.cppm
            "${CMAKE_CURRENT_BINARY_DIR}/version.cppm"
    PRIVATE
        check.cpp
        log.cpp)

target_compile_features(ngine_core PUBLIC cxx_std_23)
ngine_set_compiler_options(ngine_core)
