module;

#include <cmath>

export module ngine.math:transform;

import ngine.core;
import :vector;
import :matrix;
import :quaternion;

// Matrizen für Verschiebung, Blick und Projektion nach den Konventionen der Engine (rechtshändig,
// Kamera blickt entlang -z, Tiefe 0 bis 1).
export namespace ngine::math {

// Matrix aus Verschiebung, Drehung und Skalierung: erst skalieren, dann drehen, dann verschieben.
[[nodiscard]] constexpr Mat4 compose(Vec3 translation, Quat rotation, Vec3 scale) noexcept {
    const Mat3 r = to_mat3(rotation);
    return Mat4::from_columns(extend(r[0] * scale.x, 0.0f), extend(r[1] * scale.y, 0.0f),
                              extend(r[2] * scale.z, 0.0f), extend(translation, 1.0f));
}

// Blickmatrix: Welt in Kamerakoordinaten, die Kamera blickt entlang -z.
[[nodiscard]] inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    const Vec3 forward = normalize(target - eye);
    const Vec3 right = normalize(cross(forward, up));
    const Vec3 true_up = cross(right, forward);
    return Mat4::from_columns({right.x, true_up.x, -forward.x, 0.0f},
                              {right.y, true_up.y, -forward.y, 0.0f},
                              {right.z, true_up.z, -forward.z, 0.0f},
                              {-dot(right, eye), -dot(true_up, eye), dot(forward, eye), 1.0f});
}

// Perspektive mit Tiefe 0 (nah) bis 1 (fern).
[[nodiscard]] inline Mat4 perspective(f32 fov_y, f32 aspect, f32 z_near, f32 z_far) noexcept {
    const f32 f = 1.0f / std::tan(fov_y * 0.5f);
    Mat4 m = Mat4::from_columns({}, {}, {}, {});
    m[0].x = f / aspect;
    m[1].y = f;
    m[2].z = z_far / (z_near - z_far);
    m[2].w = -1.0f;
    m[3].z = z_near * z_far / (z_near - z_far);
    return m;
}

// Perspektive mit umgekehrter Tiefe und unendlich fernem Horizont: Tiefe 1 an der nahen Ebene,
// 0 im Unendlichen. Mit einem Tiefenpuffer in 32-Bit-Gleitkomma verteilt das die Genauigkeit
// gleichmäßig über die Entfernung, wie es große Welten brauchen. Tiefentest dann mit „größer“.
[[nodiscard]] inline Mat4 perspective_reverse_z(f32 fov_y, f32 aspect, f32 z_near) noexcept {
    const f32 f = 1.0f / std::tan(fov_y * 0.5f);
    Mat4 m = Mat4::from_columns({}, {}, {}, {});
    m[0].x = f / aspect;
    m[1].y = f;
    m[2].w = -1.0f;
    m[3].z = z_near;
    return m;
}

// Parallelprojektion (z. B. für Schattenkarten) mit Tiefe 0 bis 1.
[[nodiscard]] constexpr Mat4 orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 z_near,
                                          f32 z_far) noexcept {
    return Mat4::from_columns({2.0f / (right - left), 0.0f, 0.0f, 0.0f},
                              {0.0f, 2.0f / (top - bottom), 0.0f, 0.0f},
                              {0.0f, 0.0f, 1.0f / (z_near - z_far), 0.0f},
                              {-(right + left) / (right - left), -(top + bottom) / (top - bottom),
                               z_near / (z_near - z_far), 1.0f});
}

} // namespace ngine::math
