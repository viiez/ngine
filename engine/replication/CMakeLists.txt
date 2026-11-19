add_library(ngine_replication)
add_library(ngine::replication ALIAS ngine_replication)

target_sources(ngine_replication
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            replication.cppm
            snapshot.cppm
            session.cppm
            input.cppm
    PRIVATE
        snapshot.cpp
        session.cpp
        input.cpp)

target_link_libraries(ngine_replication
    PUBLIC ngine::core ngine::reflect ngine::world ngine::net ngine::input
    PRIVATE ngine::serialize ngine::profile)
ngine_set_compiler_options(ngine_replication)
