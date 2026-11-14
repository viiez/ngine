add_library(ngine_script)
add_library(ngine::script ALIAS ngine_script)

target_sources(ngine_script
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            script.cppm
            bindings.cppm
    PRIVATE
        FILE_SET internal TYPE CXX_MODULES
        FILES
            native.cppm
            world_api.cppm
            script_api.cppm
    PRIVATE
        runtime.cpp
        bindings.cpp
        world_api.cpp
        script_api.cpp)

# Plattformcode nur in den Adaptern: je eine Datei pro Betriebssystem.
if(WIN32)
    target_sources(ngine_script PRIVATE native_windows.cpp)
else()
    target_sources(ngine_script PRIVATE native_linux.cpp)
    target_link_libraries(ngine_script PRIVATE ${CMAKE_DL_LIBS})
endif()

# Zur Laufzeit wird hostfxr dynamisch geladen; zum Bauen braucht der C++-Teil kein .NET.
target_link_libraries(ngine_script PUBLIC ngine::core ngine::reflect ngine::world
    PRIVATE ngine::profile ngine::serialize)
ngine_set_compiler_options(ngine_script)

# Der verwaltete Teil (NGine.Scripting) braucht das .NET-SDK. Fehlt es, baut die Engine ohne ihn,
# und die Laufzeit meldet beim Start, dass die Assembly fehlt.
if(NGINE_WITH_DOTNET)
    # ngine_bindgen (R4.4) schreibt die C#-Structs der Komponenten aus der Reflection der Engine.
    add_executable(ngine_bindgen bindgen/main.cpp)
    target_link_libraries(ngine_bindgen PRIVATE
        ngine::core ngine::filesystem ngine::reflect ngine::world ngine::physics ngine::render
        ngine::script)
    ngine_set_compiler_options(ngine_bindgen)

    set(generated "${CMAKE_BINARY_DIR}/generated/csharp")
    add_custom_command(
        OUTPUT "${generated}/Components.g.cs"
        COMMAND ngine_bindgen "${generated}/Components.g.cs"
        DEPENDS ngine_bindgen
        COMMENT "C#-Bindings aus der Reflection"
        VERBATIM)

    ngine_add_dotnet_project(ngine_script_managed
        PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/managed/NGine.Scripting.csproj"
        SOURCES managed/Host.cs managed/Log.cs managed/Native.cs managed/Script.cs
            managed/Components.cs managed/Math.cs managed/Entity.cs managed/World.cs
            managed/ScriptClasses.cs
        GENERATED "${generated}/Components.g.cs"
        PROPERTIES "NgineGeneratedDir=${generated}")
endif()
