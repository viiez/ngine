add_executable(ngine_sandbox main.cpp)
target_link_libraries(ngine_sandbox PRIVATE ngine::core)
ngine_set_compiler_options(ngine_sandbox)
