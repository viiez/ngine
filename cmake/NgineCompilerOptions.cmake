# Gemeinsame Compiler-Einstellungen für alle NGine-Ziele.
# Aufruf: ngine_set_compiler_options(<target>)

function(ngine_set_compiler_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        # C4324 meldet Auffüllung durch alignas. Die ist bei Cache-Zeilen-Ausrichtung gewollt
        # (Job-System, Allokatoren).
        target_compile_options(${target} PRIVATE /wd4324)
        if(NGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wconversion -Wsign-conversion -Wshadow
            -Wnon-virtual-dtor -Wold-style-cast)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # Teilweise Designated Initializers mit Nullwerten sind bei Vulkan-Strukturen gewollt.
            target_compile_options(${target} PRIVATE -Wno-missing-designated-field-initializers)
        endif()
        if(NGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
