add_library(ngine_jobs)
add_library(ngine::jobs ALIAS ngine_jobs)

target_sources(ngine_jobs
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            jobs.cppm
            deque.cppm
            function.cppm
            system.cppm
            graph.cppm
    PRIVATE
        system.cpp
        graph.cpp)

find_package(Threads REQUIRED)
target_link_libraries(ngine_jobs
    PUBLIC ngine::core ngine::memory
    PRIVATE ngine::profile
    PRIVATE Threads::Threads)
ngine_set_compiler_options(ngine_jobs)
