find_package(fastgltf CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(Ktx CONFIG REQUIRED)
# KTX::ktx gibt im Debug-Build _DEBUG und DEBUG an alle weiter, die es linken. Für C++-Module
# müssen die Definitionen aber zu denen der importierten Module passen (MSVC C5050). ngine_ktx
# reicht nur Bibliothek, Include-Pfad und die übrigen Definitionen weiter (auch für ngine.cook).
if(NOT TARGET ngine_ktx)
    add_library(ngine_ktx INTERFACE)
    target_link_libraries(ngine_ktx INTERFACE $<LINK_ONLY:KTX::ktx>)
    target_include_directories(ngine_ktx SYSTEM INTERFACE
        $<TARGET_PROPERTY:KTX::ktx,INTERFACE_INCLUDE_DIRECTORIES>)
    get_target_property(ngine_ktx_definitions KTX::ktx INTERFACE_COMPILE_DEFINITIONS)
    if(ngine_ktx_definitions)
        list(FILTER ngine_ktx_definitions EXCLUDE REGEX "DEBUG")
        target_compile_definitions(ngine_ktx INTERFACE ${ngine_ktx_definitions})
    endif()
endif()

add_library(ngine_asset)
add_library(ngine::asset ALIAS ngine_asset)

target_sources(ngine_asset
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            asset.cppm
            model.cppm
            image.cppm
            geometry.cppm
            database.cppm
            resources.cppm
            cooked.cppm
    PRIVATE
        database.cpp
        resources.cpp
        geometry.cpp
        gltf_fastgltf.cpp
        image_stb.cpp
        mips.cpp
        cooked.cpp
        ktx_libktx.cpp)

# fastgltf steckt nur in gltf_fastgltf.cpp, stb_image nur in image_stb.cpp (ADR 0027). Alle
# anderen sehen nur Model und Image in reinen CPU-Daten. Asset-Datenbank und Ressourcen-Manager
# (R2.4, ADR 0033) lesen Ordner über ngine.filesystem und laden im Hintergrund über ngine.jobs.
# KTX-Software steckt nur in ktx_libktx.cpp (lesen und umwandeln, ADR 0036); Kochen liegt in
# ngine.cook.
target_include_directories(ngine_asset SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(ngine_asset
    PUBLIC ngine::core ngine::math ngine::jobs
    PRIVATE ngine::filesystem ngine::profile ngine::serialize fastgltf::fastgltf ngine_ktx)
ngine_set_compiler_options(ngine_asset)
