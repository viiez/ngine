# Shader zur Laufzeit übersetzen (R2.4, ADR 0034): nur für Werkzeuge wie den Editor.
find_package(slang CONFIG REQUIRED)

add_library(ngine_shader_compiler)
add_library(ngine::shader_compiler ALIAS ngine_shader_compiler)

target_sources(ngine_shader_compiler
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            shader_compiler.cppm
    PRIVATE
        compiler.cpp)

# Slang steckt nur in compiler.cpp; die Bibliothek ist groß und wird nur von Editor und Tests
# gelinkt.
target_link_libraries(ngine_shader_compiler
    PUBLIC ngine::core ngine::rhi
    PRIVATE ngine::filesystem ngine::profile slang::slang)
ngine_set_compiler_options(ngine_shader_compiler)

# Unter Windows lädt Slang weitere DLLs (z. B. slang-glslang-*.dll) und seine Standardmodule
# (slang-standard-module-*, slang.slang, gfx.slang) erst zur Laufzeit neben seiner DLL; vcpkg kopiert
# davon nur die gelinkten DLLs. ngine_copy_slang_runtime(<ziel>) holt den Rest nach, für Debug aus
# debug/bin. Unter Linux findet Slang alles neben libslang.so im vcpkg-Ordner.
function(ngine_copy_slang_runtime target)
    if(WIN32)
        set(slang_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        set(slang_bin "${slang_root}/$<$<CONFIG:Debug>:debug/>bin")
        file(GLOB slang_entries RELATIVE "${slang_root}/bin"
            "${slang_root}/bin/slang*" "${slang_root}/bin/gfx.slang")
        foreach(entry IN LISTS slang_entries)
            if(IS_DIRECTORY "${slang_root}/bin/${entry}")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                        "${slang_bin}/${entry}" "$<TARGET_FILE_DIR:${target}>/${entry}"
                    VERBATIM)
            elseif(NOT entry MATCHES "\\.pdb$")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${slang_bin}/${entry}" "$<TARGET_FILE_DIR:${target}>"
                    VERBATIM)
            endif()
        endforeach()
    endif()
endfunction()
