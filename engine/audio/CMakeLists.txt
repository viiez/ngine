add_library(ngine_audio)
add_library(ngine::audio ALIAS ngine_audio)

target_sources(ngine_audio
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            audio.cppm
            types.cppm
            queue.cppm
            spatial.cppm
            decoder.cppm
            stream.cppm
            mixer.cppm
            device.cppm
            sound.cppm
            engine.cppm
            components.cppm
            system.cppm
    PRIVATE
        spatial.cpp
        stream.cpp
        mixer.cpp
        device_null.cpp
        sound.cpp
        engine.cpp
        components.cpp
        system.cpp)

target_link_libraries(ngine_audio
    PUBLIC ngine::core ngine::math ngine::reflect ngine::asset ngine::world
    PRIVATE ngine::filesystem ngine::serialize)

find_package(Threads REQUIRED)
target_link_libraries(ngine_audio PRIVATE Threads::Threads)

# Gerät und Dekodieren über miniaudio (ADR 0049); ohne NGINE_WITH_AUDIO nur das Null-Gerät.
if(NGINE_WITH_AUDIO)
    find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
    find_path(STB_INCLUDE_DIR stb_vorbis.c REQUIRED)
    target_sources(ngine_audio PRIVATE miniaudio.cpp miniaudio_impl.cpp)
    target_include_directories(ngine_audio SYSTEM PRIVATE ${MINIAUDIO_INCLUDE_DIR} ${STB_INCLUDE_DIR})
    # Code der Fremdbibliothek: ohne Warnungen übersetzen.
    if(MSVC)
        set_source_files_properties(miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
    else()
        set_source_files_properties(miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    if(UNIX AND NOT APPLE)
        # miniaudio lädt PipeWire, PulseAudio und ALSA zur Laufzeit nach.
        target_link_libraries(ngine_audio PRIVATE ${CMAKE_DL_LIBS} m)
    endif()
else()
    target_sources(ngine_audio PRIVATE miniaudio_null.cpp)
endif()
ngine_set_compiler_options(ngine_audio)
