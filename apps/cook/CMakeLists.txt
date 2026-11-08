# ngine_cook (R2.5): kocht die Modelle eines Asset-Ordners in den Cache, z. B. vor einem Build des
# Spiels oder in der CI.
add_executable(ngine_cook_tool main.cpp)
set_target_properties(ngine_cook_tool PROPERTIES OUTPUT_NAME ngine_cook)
target_link_libraries(ngine_cook_tool PRIVATE
    ngine::core ngine::asset ngine::cook ngine::filesystem ngine_app_common)
ngine_set_compiler_options(ngine_cook_tool)
