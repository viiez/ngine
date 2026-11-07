add_library(ngine_edit)
add_library(ngine::edit ALIAS ngine_edit)

target_sources(ngine_edit
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            edit.cppm
            document.cppm
            commands.cppm
            selection.cppm
            inspector.cppm
            tree.cppm
            gizmo.cppm
    PRIVATE
        document.cpp
        commands.cpp
        inspector.cpp
        tree.cpp
        gizmo.cpp)

# Editor-Logik ohne UI, Fenster und GPU (R2.2, Gizmos und Spielen seit R2.3); der Editor und die Tests bauen darauf auf.
target_link_libraries(ngine_edit
    PUBLIC ngine::core ngine::math ngine::reflect ngine::world
    PRIVATE ngine::serialize)
ngine_set_compiler_options(ngine_edit)
