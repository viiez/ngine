add_library(ngine_voxel)
add_library(ngine::voxel ALIAS ngine_voxel)

target_sources(ngine_voxel
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            voxel.cppm
            grid.cppm
            chunk.cppm
            mesh.cppm
            meshing.cppm
            edit.cppm
            noise.cppm
            generator.cppm
    PRIVATE
        grid.cpp
        chunk.cpp
        surface_nets.cpp
        meshing.cpp
        edit.cpp
        noise.cpp
        generator.cpp)

# Läuft ohne GPU: Der Server braucht Dichtefeld und Bearbeitung, nur das Zeichnen nicht. Seit
# R3.1 laufen Füllen und Meshing vieler Chunks im Job-System.
target_link_libraries(ngine_voxel PUBLIC ngine::core ngine::math ngine::jobs PRIVATE ngine::profile)
ngine_set_compiler_options(ngine_voxel)
# Der Generator muss auf allen Plattformen bitgleich rechnen (R3.2, ADR 0039): keine
# zusammengezogenen Multiplikationen und Additionen (FMA). MSVC erzeugt sie unter /fp:precise seit
# Visual Studio 2022 ohne /fp:contract nicht mehr; die Option steht trotzdem ausdrücklich da.
if(MSVC)
    target_compile_options(ngine_voxel PRIVATE /fp:precise)
else()
    target_compile_options(ngine_voxel PRIVATE -ffp-contract=off)
endif()
