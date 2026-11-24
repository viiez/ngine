add_library(ngine_replication)
add_library(ngine::replication ALIAS ngine_replication)

target_sources(ngine_replication
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            replication.cppm
            schema.cppm
            snapshot.cppm
            session.cppm
            input.cppm
            prediction.cppm
            wire.cppm
            terrain.cppm
    PRIVATE
        schema.cpp
        snapshot.cpp
        encoder.cpp
        mirror.cpp
        session.cpp
        input.cpp
        prediction.cpp
        terrain.cpp)

target_link_libraries(ngine_replication
    PUBLIC ngine::core ngine::math ngine::reflect ngine::world ngine::net ngine::input
        ngine::serialize
    PRIVATE ngine::profile)
ngine_set_compiler_options(ngine_replication)
