module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

export module ngine.math:transform;

import ngine.core;
import :types;

// Drehungen, Matrizen und Projektionen. Die Fassungen hier legen die Konventionen der Engine fest
// (rechtshändig, Tiefe 0 bis 1), damit niemand die passende GLM-Variante suchen muss.
export namespace ngine::math {

template <typename T>
[[nodiscard]] constexpr T lerp(T a, T b, f32 t) noexcept {
    return a + (b - a) * t;
}

template <typename V>
[[nodiscard]] constexpr auto length_squared(V v) noexcept {
    return glm::dot(v, v);
}

// Drehung um axis (normiert) um angle im Bogenmaß, gegen den Uhrzeigersinn, wenn axis auf den
// Betrachter zeigt.
[[nodiscard]] inline Quat angle_axis(f32 angle, Vec3 axis) noexcept {
    return glm::angleAxis(angle, axis);
}

// Drehung, die -z auf forward und y möglichst auf up abbildet (Blickrichtung einer Kamera).
[[nodiscard]] inline Quat look_rotation(Vec3 forward, Vec3 up) noexcept {
    return glm::quatLookAtRH(glm::normalize(forward), up);
}

[[nodiscard]] inline Mat3 to_mat3(Quat q) noexcept {
    return glm::mat3_cast(q);
}
[[nodiscard]] inline Mat4 to_mat4(Quat q) noexcept {
    return glm::mat4_cast(q);
}

// Matrix aus Verschiebung, Drehung und Skalierung: erst skalieren, dann drehen, dann verschieben.
[[nodiscard]] inline Mat4 compose(Vec3 translation, Quat rotation, Vec3 scale) noexcept {
    Mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = Vec4{translation, 1.0f};
    return m;
}

// Blickmatrix: Welt in Kamerakoordinaten, die Kamera blickt entlang -z.
[[nodiscard]] inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    return glm::lookAtRH(eye, target, up);
}

// Perspektive mit Tiefe 0 (nah) bis 1 (fern).
[[nodiscard]] inline Mat4 perspective(f32 fov_y, f32 aspect, f32 z_near, f32 z_far) noexcept {
    return glm::perspectiveRH_ZO(fov_y, aspect, z_near, z_far);
}

// Perspektive mit umgekehrter Tiefe und unendlich fernem Horizont: Tiefe 1 an der nahen Ebene,
// 0 im Unendlichen. Mit einem Tiefenpuffer in 32-Bit-Gleitkomma verteilt das die Genauigkeit
// gleichmäßig über die Entfernung, wie es große Welten brauchen. Tiefentest dann mit „größer“.
[[nodiscard]] inline Mat4 perspective_reverse_z(f32 fov_y, f32 aspect, f32 z_near) noexcept {
    const f32 f = 1.0f / std::tan(fov_y * 0.5f);
    Mat4 m{0.0f};
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][3] = -1.0f;
    m[3][2] = z_near;
    return m;
}

// Parallelprojektion (z. B. für Schattenkarten) mit Tiefe 0 bis 1.
[[nodiscard]] inline Mat4 orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 z_near,
                                       f32 z_far) noexcept {
    return glm::orthoRH_ZO(left, right, bottom, top, z_near, z_far);
}

// Vergleiche mit absoluter Toleranz, für Tests und Prüfungen.
[[nodiscard]] inline bool near(f32 a, f32 b, f32 epsilon) noexcept {
    return std::abs(a - b) <= epsilon;
}
[[nodiscard]] inline bool near(f64 a, f64 b, f64 epsilon) noexcept {
    return std::abs(a - b) <= epsilon;
}
[[nodiscard]] inline bool near(Vec3 a, Vec3 b, f32 epsilon) noexcept {
    return near(a.x, b.x, epsilon) && near(a.y, b.y, epsilon) && near(a.z, b.z, epsilon);
}
[[nodiscard]] inline bool near(Vec4 a, Vec4 b, f32 epsilon) noexcept {
    return near(a.x, b.x, epsilon) && near(a.y, b.y, epsilon) && near(a.z, b.z, epsilon) &&
           near(a.w, b.w, epsilon);
}
[[nodiscard]] inline bool near(DVec3 a, DVec3 b, f64 epsilon) noexcept {
    return near(a.x, b.x, epsilon) && near(a.y, b.y, epsilon) && near(a.z, b.z, epsilon);
}
// Gleiche Drehung: q und -q drehen gleich.
[[nodiscard]] inline bool near(Quat a, Quat b, f32 epsilon) noexcept {
    return std::abs(std::abs(glm::dot(a, b)) - 1.0f) <= epsilon;
}
[[nodiscard]] inline bool near(const Mat3& a, const Mat3& b, f32 epsilon) noexcept {
    for (int c = 0; c < 3; ++c) {
        if (!near(a[c], b[c], epsilon)) {
            return false;
        }
    }
    return true;
}
[[nodiscard]] inline bool near(const Mat4& a, const Mat4& b, f32 epsilon) noexcept {
    for (int c = 0; c < 4; ++c) {
        if (!near(a[c], b[c], epsilon)) {
            return false;
        }
    }
    return true;
}

} // namespace ngine::math
