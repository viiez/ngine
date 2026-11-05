module;

#include <optional>

// Komponenten des Renderers in der Reflection und die Kamera der Welt.
module ngine.render;

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.world;

namespace ngine::render {

namespace {

// Drehung aus dem linearen Teil einer Lage, ohne Skalierung (Spalten normiert, rechtwinklig).
math::Mat3 orthonormal(const math::Mat3& linear) {
    const math::Vec3 z = math::normalize(linear[2]);
    const math::Vec3 x = math::normalize(math::cross(linear[1], z));
    const math::Vec3 y = math::cross(z, x);
    return math::Mat3::from_columns(x, y, z);
}

} // namespace

void register_types(reflect::TypeRegistry& types) {
    if (types.find<Camera>() == nullptr) {
        types.add<Camera>("Camera")
            .field("fov_y", &Camera::fov_y)
            .field("near_plane", &Camera::near_plane)
            .field("exposure", &Camera::exposure)
            .field("active", &Camera::active);
    }
    if (types.find<DirectionalLight>() == nullptr) {
        types.add<DirectionalLight>("DirectionalLight")
            .field("color", &DirectionalLight::color)
            .field("intensity", &DirectionalLight::intensity)
            .field("cast_shadows", &DirectionalLight::cast_shadows)
            .field("active", &DirectionalLight::active);
    }
    if (types.find<PointLight>() == nullptr) {
        types.add<PointLight>("PointLight")
            .field("color", &PointLight::color)
            .field("intensity", &PointLight::intensity)
            .field("range", &PointLight::range)
            .field("active", &PointLight::active);
    }
    if (types.find<SpotLight>() == nullptr) {
        types.add<SpotLight>("SpotLight")
            .field("color", &SpotLight::color)
            .field("intensity", &SpotLight::intensity)
            .field("range", &SpotLight::range)
            .field("inner_angle", &SpotLight::inner_angle)
            .field("outer_angle", &SpotLight::outer_angle)
            .field("active", &SpotLight::active);
    }
    if (types.find<MeshRenderer>() == nullptr) {
        types.add<MeshRenderer>("MeshRenderer")
            .field("mesh", &MeshRenderer::mesh)
            .field("material", &MeshRenderer::material)
            .transient();
    }
}

std::optional<CameraView> find_camera(world::World& world) {
    std::optional<CameraView> found;
    world.each<const world::WorldTransform, const Camera>(
        [&](const world::WorldTransform& transform, const Camera& camera) {
            if (found || !camera.active) {
                return;
            }
            found = CameraView{.position = transform.position(),
                               .rotation = orthonormal(transform.value.linear),
                               .camera = camera};
        });
    return found;
}

math::Mat4 view_projection(const CameraView& view, f32 aspect) {
    // Kamerarelativ: Die Blickmatrix ist nur die umgekehrte Drehung (ADR 0012).
    const math::Mat4 rotation = math::to_mat4(math::transpose(view.rotation));
    return math::perspective_reverse_z(view.camera.fov_y, aspect, view.camera.near_plane) *
           rotation;
}

} // namespace ngine::render
