find_package(fastgltf CONFIG REQUIRED)
find_package(Stb REQUIRED)

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
    PRIVATE
        database.cpp
        resources.cpp
        geometry.cpp
        gltf_fastgltf.cpp
        image_stb.cpp
        mips.cpp)

# fastgltf steckt nur in gltf_fastgltf.cpp, stb_image nur in image_stb.cpp (ADR 0027). Alle
# anderen sehen nur Model und Image in reinen CPU-Daten. Asset-Datenbank und Ressourcen-Manager
# (R2.4, ADR 0033) lesen Ordner über ngine.filesystem und laden im Hintergrund über ngine.jobs.
target_include_directories(ngine_asset SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(ngine_asset
    PUBLIC ngine::core ngine::math ngine::jobs
    PRIVATE ngine::filesystem ngine::profile ngine::serialize fastgltf::fastgltf)
ngine_set_compiler_options(ngine_asset)
