cmake_minimum_required(VERSION 3.30)

project(NGine
    VERSION 0.1.0
    DESCRIPTION "Eine moderne 3D-Engine mit Vulkan und Voxel-Terrain"
    LANGUAGES CXX)

if(NOT CMAKE_GENERATOR MATCHES "Ninja|Visual Studio")
    message(FATAL_ERROR "NGine nutzt C++-Module und braucht Ninja oder Visual Studio als Generator.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NGINE_BUILD_TESTS "Tests bauen" ON)
option(NGINE_BUILD_SANDBOX "Sandbox-Anwendung bauen" ON)
option(NGINE_WARNINGS_AS_ERRORS "Warnungen als Fehler behandeln" OFF)
# Ohne Fenster entsteht ein Build ohne SDL3, z. B. für Server oder reine Tests.
option(NGINE_WITH_WINDOW "Fenster-Unterstützung (SDL3) bauen" ON)
# Sanitizer für alle Ziele (Clang und GCC), z. B. "thread" oder "address,undefined".
set(NGINE_SANITIZER "" CACHE STRING "Sanitizer für alle Ziele (leer: keiner)")

if(NGINE_SANITIZER)
    if(MSVC)
        message(FATAL_ERROR "NGINE_SANITIZER wird nur mit Clang und GCC unterstützt.")
    endif()
    add_compile_options(-fsanitize=${NGINE_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${NGINE_SANITIZER})
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NgineCompilerOptions)
include(NgineShaders)

add_subdirectory(engine)

if(NGINE_BUILD_SANDBOX)
    add_subdirectory(apps/sandbox)
endif()

if(NGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
