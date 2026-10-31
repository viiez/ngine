add_library(ngine_memory)
add_library(ngine::memory ALIAS ngine_memory)

target_sources(ngine_memory
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            memory.cppm
            align.cppm
            budget.cppm
            linear.cppm
            frame.cppm
            pool.cppm
    PRIVATE
        budget.cpp
        linear.cpp
        frame.cpp
        pool.cpp)

target_link_libraries(ngine_memory PUBLIC ngine::core)
ngine_set_compiler_options(ngine_memory)
