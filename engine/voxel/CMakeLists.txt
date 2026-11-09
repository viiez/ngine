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
# zusammengezogenen Multiplikationen und Additionen (FMA). MSVC zieht unter seiner Vorgabe
# /fp:precise nichts zusammen; Clang und GCC bekommen -ffp-contract=off. Die Option gilt nur für die
# Implementierungen, nicht für das ganze Ziel: Mit anderen Optionen als ngine.math müsste CMake die
# importierten Module für ngine_voxel ein zweites Mal übersetzen, und wer beide importiert, fände
# zwei Fassungen von ngine.math.
if(NOT MSVC)
    set_source_files_properties(noise.cpp generator.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
