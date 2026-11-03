add_library(ngine_math)
add_library(ngine::math ALIAS ngine_math)

target_sources(ngine_math
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            math.cppm
            scalar.cppm
            vector.cppm
            matrix.cppm
            quaternion.cppm
            transform.cppm
            affine.cppm)

# Core-Schicht, eigene Implementierung ohne Fremdbibliothek (ADR 0025). GLM dient nur noch den
# Tests als Vergleich.
target_link_libraries(ngine_math PUBLIC ngine::core)
ngine_set_compiler_options(ngine_math)
