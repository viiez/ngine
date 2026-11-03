module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

export module ngine.math:types;

import ngine.core;

// Typen und Funktionen aus GLM unter Engine-Namen (ADR 0025). Funktionen, die in GLM schon klein
// geschrieben sind, reichen wir unverändert weiter; Namen in camelCase bekommen in :transform eine
// Fassung in snake_case.
export namespace ngine::math {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
// Weltpositionen in 64 Bit (ADR 0012): auf 10 000 km vom Ursprung noch genauer als ein Nanometer.
using DVec3 = glm::dvec3;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using DMat3 = glm::dmat3;
// Speicherreihenfolge x, y, z, w; der Konstruktor nimmt aber Quat{w, x, y, z}.
using Quat = glm::quat;

inline constexpr f32 pi = 3.14159265358979323846f;
inline constexpr f64 pi_f64 = 3.14159265358979323846;

// Umwandlungen und Einheitswerte als Funktionen statt über Konstruktoren von GLM, damit eine
// spätere eigene Implementierung nur diese Namen nachbauen muss (ADR 0025).
[[nodiscard]] constexpr Vec3 to_f32(DVec3 v) noexcept {
    return Vec3{static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
}
[[nodiscard]] constexpr DVec3 to_f64(Vec3 v) noexcept {
    return DVec3{v.x, v.y, v.z};
}
[[nodiscard]] constexpr Vec3 xyz(Vec4 v) noexcept {
    return Vec3{v.x, v.y, v.z};
}
[[nodiscard]] constexpr Vec4 extend(Vec3 v, f32 w) noexcept {
    return Vec4{v.x, v.y, v.z, w};
}
[[nodiscard]] constexpr Quat quat_identity() noexcept {
    return Quat{1.0f, 0.0f, 0.0f, 0.0f};
}
[[nodiscard]] constexpr Mat3 mat3_identity() noexcept {
    return Mat3{1.0f};
}
[[nodiscard]] constexpr Mat4 mat4_identity() noexcept {
    return Mat4{1.0f};
}

using glm::abs;
using glm::clamp;
using glm::conjugate;
using glm::cross;
using glm::degrees;
using glm::determinant;
using glm::distance;
using glm::dot;
using glm::inverse;
using glm::length;
using glm::max;
using glm::min;
using glm::normalize;
using glm::radians;
using glm::slerp;
using glm::transpose;

} // namespace ngine::math

// Operatoren findet der Compiler über die argumentabhängige Suche im Namensraum glm. Damit sie in
// importierenden Modulen sichtbar sind, müssen sie exportiert werden (so macht es auch das Modul,
// das GLM selbst mitbringt).
export namespace glm {

using glm::operator+;
using glm::operator-;
using glm::operator*;
using glm::operator/;
using glm::operator==;
using glm::operator!=;

} // namespace glm
