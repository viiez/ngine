add_library(ngine_serialize)
add_library(ngine::serialize ALIAS ngine_serialize)

target_sources(ngine_serialize
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            serialize.cppm
            json.cppm
            binary.cppm
            reflected.cppm
    PRIVATE
        json_parse.cpp
        json_write.cpp
        json_value.cpp
        reflected.cpp)

target_link_libraries(ngine_serialize PUBLIC ngine::core ngine::reflect)
ngine_set_compiler_options(ngine_serialize)
