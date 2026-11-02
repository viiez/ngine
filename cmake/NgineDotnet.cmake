# Baut C#-Projekte mit dem .NET-SDK (D2, ADR 0023). Alle Assemblies landen in einem gemeinsamen
# Ordner NGINE_DOTNET_DIR; Zwischendateien (obj, bin) im Build-Verzeichnis, nicht in den Quellen.

if(NGINE_WITH_DOTNET)
    find_program(NGINE_DOTNET dotnet DOC ".NET-SDK")
    if(NOT NGINE_DOTNET)
        message(WARNING "NGine: dotnet nicht gefunden, C#-Scripting wird ohne Assemblies gebaut. "
            "Unter Ubuntu 24.04: sudo apt install dotnet-sdk-10.0")
        set(NGINE_WITH_DOTNET OFF CACHE BOOL "C#-Scripting mit dem .NET-SDK bauen" FORCE)
    endif()
endif()

set(NGINE_DOTNET_DIR "${CMAKE_BINARY_DIR}/dotnet" CACHE INTERNAL "Ausgabeordner der C#-Assemblies")

# ngine_add_dotnet_project(<target> PROJECT <csproj> SOURCES <datei.cs>... [DEPENDS <target>...])
# Projekte mit DEPENDS bauen ihre Projektverweise nicht selbst, damit zwei Builds nicht
# gleichzeitig dieselben Dateien schreiben.
function(ngine_add_dotnet_project target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PROJECT" "SOURCES;DEPENDS")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}.stamp")
    set(build_references "true")
    if(arg_DEPENDS)
        set(build_references "false")
    endif()
    list(TRANSFORM arg_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${NGINE_DOTNET}" build "${arg_PROJECT}"
            --nologo --verbosity quiet
            --configuration $<IF:$<CONFIG:Debug>,Debug,Release>
            --artifacts-path "${CMAKE_BINARY_DIR}/dotnet-artifacts"
            --output "${NGINE_DOTNET_DIR}"
            -p:BuildProjectReferences=${build_references}
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${arg_PROJECT}" ${arg_SOURCES}
        COMMENT ".NET: ${target}"
        VERBATIM)
    add_custom_target(${target} ALL DEPENDS "${stamp}")
    if(arg_DEPENDS)
        add_dependencies(${target} ${arg_DEPENDS})
    endif()
endfunction()
