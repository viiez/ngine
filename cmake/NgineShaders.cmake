# Kompiliert Slang-Shader beim Bauen zu SPIR-V und bettet sie als C++-Daten ein.

find_program(NGINE_SLANGC slangc
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shader-slang"
    DOC "Slang-Compiler")
if(NOT NGINE_SLANGC)
    message(FATAL_ERROR "slangc nicht gefunden. Über vcpkg (shader-slang) installieren oder NGINE_SLANGC setzen.")
endif()

# ngine_compile_shader(<target> SOURCE <datei.slang> ENTRY <name> STAGE <vertex|fragment> NAME <name>)
# Erzeugt <name>.spv und <name>.spv.inc im Build-Verzeichnis von <target> und hängt
# die .inc-Datei an das Ziel, damit sie vor dem Kompilieren entsteht.
function(ngine_compile_shader target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "SOURCE;ENTRY;STAGE;NAME" "")
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${arg_SOURCE}")
    set(spirv "${CMAKE_CURRENT_BINARY_DIR}/${arg_NAME}.spv")
    set(include "${CMAKE_CURRENT_BINARY_DIR}/${arg_NAME}.spv.inc")

    add_custom_command(
        OUTPUT "${spirv}"
        COMMAND "${NGINE_SLANGC}" "${source}"
            -target spirv
            -entry "${arg_ENTRY}" -stage "${arg_STAGE}"
            -fvk-use-entrypoint-name
            -o "${spirv}"
        DEPENDS "${source}"
        COMMENT "Slang: ${arg_SOURCE} (${arg_ENTRY}) -> SPIR-V"
        VERBATIM)

    add_custom_command(
        OUTPUT "${include}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${spirv} -DOUTPUT=${include}
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS "${spirv}" "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        VERBATIM)

    target_sources(${target} PRIVATE "${include}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
