find_package(GameNetworkingSockets CONFIG REQUIRED)

add_library(ngine_net)
add_library(ngine::net ALIAS ngine_net)

target_sources(ngine_net
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            net.cppm
    PRIVATE
        transport_gns.cpp)

# GameNetworkingSockets steckt nur in transport_gns.cpp (ADR 0009, ADR 0024). vcpkg baut unter
# Linux eine statische, unter Windows eine dynamische Bibliothek.
target_link_libraries(ngine_net
    PUBLIC ngine::core
    PRIVATE ngine::profile
            $<IF:$<TARGET_EXISTS:GameNetworkingSockets::GameNetworkingSockets>,GameNetworkingSockets::GameNetworkingSockets,GameNetworkingSockets::GameNetworkingSockets_s>)
ngine_set_compiler_options(ngine_net)
