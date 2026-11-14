add_library(ngine_input)
add_library(ngine::input ALIAS ngine_input)

target_sources(ngine_input
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            input.cppm
            actions.cppm
    PRIVATE
        actions.cpp)

# Nur die Ereignistypen der Plattform; ohne NGINE_WITH_WINDOW baut ngine_platform ohne SDL3.
target_link_libraries(ngine_input
    PUBLIC ngine::core ngine::platform ngine::reflect
    PRIVATE ngine::filesystem ngine::serialize)
ngine_set_compiler_options(ngine_input)
