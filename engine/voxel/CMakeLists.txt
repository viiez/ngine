add_library(ngine_voxel)
add_library(ngine::voxel ALIAS ngine_voxel)

target_sources(ngine_voxel
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            voxel.cppm
            grid.cppm
            mesh.cppm
            edit.cppm
    PRIVATE
        grid.cpp
        surface_nets.cpp
        edit.cpp)

# Läuft ohne GPU: Der Server braucht Dichtefeld und Bearbeitung, nur das Zeichnen nicht.
target_link_libraries(ngine_voxel PUBLIC ngine::core ngine::math PRIVATE ngine::profile)
ngine_set_compiler_options(ngine_voxel)
