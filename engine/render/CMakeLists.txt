add_library(ngine_render)
add_library(ngine::render ALIAS ngine_render)

target_sources(ngine_render
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            render.cppm
            pool.cppm
            profiler.cppm
            graph.cppm
            components.cppm
            lights.cppm
            shadows.cppm
            debug_draw.cppm
            materials.cppm
            geometry.cppm
            meshes.cppm
            terrain.cppm
            renderer.cppm
    PRIVATE
        pool.cpp
        profiler.cpp
        graph.cpp
        stores.cpp
        geometry.cpp
        terrain.cpp
        lights.cpp
        shadows.cpp
        debug_draw.cpp
        renderer.cpp
        scene.cpp
        models.cpp)

# Laufzeit-Schicht: kennt die RHI (nie Vulkan), die Welt für Kamera und MeshRenderer und die
# Ressourcen-Schicht für Modelle.
target_link_libraries(ngine_render
    PUBLIC ngine::core ngine::math ngine::rhi ngine::reflect ngine::world ngine::asset
    PRIVATE ngine::profile ngine::shaders)
ngine_set_compiler_options(ngine_render)
