add_library(ngine_script)
add_library(ngine::script ALIAS ngine_script)

target_sources(ngine_script
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            script.cppm
    PRIVATE
        FILE_SET internal TYPE CXX_MODULES
        FILES
            native.cppm
    PRIVATE
        runtime.cpp)

# Plattformcode nur in den Adaptern: je eine Datei pro Betriebssystem.
if(WIN32)
    target_sources(ngine_script PRIVATE native_windows.cpp)
else()
    target_sources(ngine_script PRIVATE native_linux.cpp)
    target_link_libraries(ngine_script PRIVATE ${CMAKE_DL_LIBS})
endif()

# Zur Laufzeit wird hostfxr dynamisch geladen; zum Bauen braucht der C++-Teil kein .NET.
target_link_libraries(ngine_script PUBLIC ngine::core PRIVATE ngine::profile)
ngine_set_compiler_options(ngine_script)

# Der verwaltete Teil (NGine.Scripting) braucht das .NET-SDK. Fehlt es, baut die Engine ohne ihn,
# und die Laufzeit meldet beim Start, dass die Assembly fehlt.
if(NGINE_WITH_DOTNET)
    ngine_add_dotnet_project(ngine_script_managed
        PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/managed/NGine.Scripting.csproj"
        SOURCES managed/Host.cs managed/Log.cs managed/Native.cs managed/Script.cs)
endif()
