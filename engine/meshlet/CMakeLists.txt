add_library(ngine_meshlet)
add_library(ngine::meshlet ALIAS ngine_meshlet)

target_sources(ngine_meshlet
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            meshlet.cppm
    PRIVATE
        meshlet.cpp)

# Ohne GPU und ohne Fremdbibliothek: Der Mesher des Terrains baut seine Meshlets in Jobs, der
# Lader von Modellen aus den Quellen ebenso. Gekochte Modelle bekommen sie aus meshoptimizer
# (ngine.cook) im selben Format.
target_link_libraries(ngine_meshlet PUBLIC ngine::core ngine::math)
ngine_set_compiler_options(ngine_meshlet)
