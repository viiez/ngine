find_package(Catch2 3 REQUIRED)

add_executable(ngine_tests
    core/log_tests.cpp
    core/types_tests.cpp
    rhi/vulkan_triangle_tests.cpp)
target_link_libraries(ngine_tests PRIVATE
    ngine::core ngine::rhi ngine::rhi_vulkan ngine::shaders Catch2::Catch2WithMain)
ngine_set_compiler_options(ngine_tests)

include(Catch)
catch_discover_tests(ngine_tests)
