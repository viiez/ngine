# Triplet für das Preset linux-clang-tsan (R5.1, TD-47): wie x64-linux, aber Jolt wird mit
# ThreadSanitizer gebaut. Sonst sähe ThreadSanitizer die Synchronisation in der Bibliothek nicht
# und meldete Wettläufe zwischen Jolts Inline-Code und der Bibliothek, die es nicht gibt. Die
# übrigen Bibliotheken bleiben ohne, ihr Code läuft nicht in mehreren Threads der Engine.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

if(PORT STREQUAL "joltphysics")
    set(VCPKG_C_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
    set(VCPKG_CXX_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
    set(VCPKG_LINKER_FLAGS "-fsanitize=thread")
endif()
