find_package(glm CONFIG REQUIRED)

add_library(ngine_math)
add_library(ngine::math ALIAS ngine_math)

target_sources(ngine_math
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            math.cppm
            types.cppm
            transform.cppm
            affine.cppm)

# GLM (ADR 0025) kennt nur dieses Modul; andere Ziele sehen es nur über die exportierten Namen.
# Schalter für GLM:
#   GLM_FORCE_DEPTH_ZERO_TO_ONE  Tiefe im Clip-Raum 0 bis 1 wie in Vulkan
#   GLM_FORCE_CTOR_INIT          Standardkonstruktoren initialisieren (Nullvektor, Einheitsmatrix,
#                                Einheitsquaternion); Komponenten dürfen keine Zufallswerte haben
#   GLM_FORCE_XYZW_ONLY          Vektoren haben nur die Felder x, y, z, w, ohne Unions, damit die
#                                Reflection sie beschreiben kann
target_compile_definitions(ngine_math PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_CTOR_INIT GLM_FORCE_XYZW_ONLY)
target_link_libraries(ngine_math PUBLIC ngine::core PRIVATE glm::glm-header-only)
ngine_set_compiler_options(ngine_math)
