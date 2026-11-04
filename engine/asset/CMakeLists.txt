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
    PRIVATE
        geometry.cpp
        gltf_fastgltf.cpp
        image_stb.cpp)

# fastgltf steckt nur in gltf_fastgltf.cpp, stb_image nur in image_stb.cpp (ADR 0027). Alle
# anderen sehen nur Model und Image in reinen CPU-Daten.
target_include_directories(ngine_asset SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(ngine_asset
    PUBLIC ngine::core ngine::math
    PRIVATE ngine::filesystem ngine::profile fastgltf::fastgltf)
ngine_set_compiler_options(ngine_asset)
