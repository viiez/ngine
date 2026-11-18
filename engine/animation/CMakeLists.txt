add_library(ngine_animation)
add_library(ngine::animation ALIAS ngine_animation)

target_sources(ngine_animation
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            animation.cppm
            rig.cppm
            graph.cppm
            machine.cppm
            components.cppm
            system.cppm
    PRIVATE
        rig.cpp
        graph.cpp
        machine.cpp
        components.cpp
        system.cpp)

# Skelett-Animation ohne GPU (R4.6, ANIM-01, ANIM-02, ADR 0050): Abtasten, Mischen, Zustandsautomat
# und das AnimationSystem in Jobs. Der Renderer liest nur die fertigen Paletten.
target_link_libraries(ngine_animation
    PUBLIC ngine::core ngine::math ngine::reflect ngine::jobs ngine::asset ngine::world
    PRIVATE ngine::filesystem ngine::serialize ngine::profile)
ngine_set_compiler_options(ngine_animation)
