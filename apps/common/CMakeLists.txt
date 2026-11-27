# Gemeinsamer Code der Anwendungen (Sandbox und Editor), z. B. die Standardszene.
add_library(ngine_app_common)
target_sources(ngine_app_common PUBLIC FILE_SET CXX_MODULES FILES
    game_world.cppm model_scene.cppm prefabs.cppm paths.cppm input_bindings.cppm physics_scene.cppm
    player.cppm prediction.cppm scripting.cppm terrain_materials.cppm net_client.cppm game_server.cppm)
target_link_libraries(ngine_app_common PUBLIC
    ngine::core ngine::math ngine::profile ngine::reflect ngine::world ngine::asset ngine::rhi
    ngine::render ngine::filesystem ngine::jobs ngine::input ngine::platform ngine::physics ngine::voxel ngine::script ngine::audio
    ngine::animation ngine::net ngine::replication)
ngine_set_compiler_options(ngine_app_common)
