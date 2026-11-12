# Asset-Pipeline (R2.5, ADR 0036): kocht Quelldateien in Laufzeitformate. Nur Editor, Werkzeug
# ngine_cook und Tests linken das Modul; Spiel und Server lesen nur den Cache.
find_package(Ktx CONFIG REQUIRED)
find_package(mikktspace CONFIG REQUIRED)

add_library(ngine_cook)
add_library(ngine::cook ALIAS ngine_cook)

target_sources(ngine_cook
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            cook.cppm
    PRIVATE
        cooker.cpp
        ktx_encode.cpp
        tangents_mikktspace.cpp)

# KTX-Software steckt nur in ktx_encode.cpp, MikkTSpace nur in tangents_mikktspace.cpp.
target_link_libraries(ngine_cook
    PUBLIC ngine::core ngine::asset ngine::jobs
    PRIVATE ngine::math ngine::filesystem ngine::profile ngine_ktx mikktspace::mikktspace)
ngine_set_compiler_options(ngine_cook)
