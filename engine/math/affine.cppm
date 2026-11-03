module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

export module ngine.math:affine;

import ngine.core;
import :types;
import :transform;

export namespace ngine::math {

// Affine Abbildung mit gemischter Genauigkeit (ADR 0012): Drehung, Skalierung und Scherung in
// 32 Bit, Verschiebung in 64 Bit. Damit bleiben Weltpositionen weit vom Ursprung genau, ohne dass
// jede Matrix doppelt so groß wird. Gezeichnet wird relativ zur Kamera (relative_to()).
struct Affine {
    Mat3 linear{1.0f};
    DVec3 translation{0.0};

    // Erst skalieren, dann drehen, dann verschieben.
    [[nodiscard]] static Affine from(DVec3 translation, Quat rotation, Vec3 scale) noexcept {
        Mat3 linear = glm::mat3_cast(rotation);
        linear[0] *= scale.x;
        linear[1] *= scale.y;
        linear[2] *= scale.z;
        return Affine{.linear = linear, .translation = translation};
    }

    [[nodiscard]] DVec3 transform_point(DVec3 point) const noexcept {
        return DMat3{linear} * point + translation;
    }
    [[nodiscard]] Vec3 transform_direction(Vec3 direction) const noexcept {
        return linear * direction;
    }

    // Matrix für die GPU, deren Ursprung in origin liegt (meist die Kameraposition). Die
    // Differenz entsteht in 64 Bit; erst das kleine Ergebnis wird auf 32 Bit gerundet.
    [[nodiscard]] Mat4 relative_to(DVec3 origin) const noexcept {
        Mat4 m{linear};
        m[3] = Vec4{Vec3{translation - origin}, 1.0f};
        return m;
    }

    // Erst b, dann a anwenden (wie a * b bei Matrizen).
    friend Affine operator*(const Affine& a, const Affine& b) noexcept {
        return Affine{.linear = a.linear * b.linear,
                      .translation = a.transform_point(b.translation)};
    }
};

[[nodiscard]] inline Affine inverse(const Affine& a) noexcept {
    const Mat3 linear = glm::inverse(a.linear);
    return Affine{.linear = linear, .translation = -(DMat3{linear} * a.translation)};
}

[[nodiscard]] inline bool near(const Affine& a, const Affine& b, f32 epsilon_linear,
                               f64 epsilon_translation) noexcept {
    return near(a.linear, b.linear, epsilon_linear) &&
           near(a.translation, b.translation, epsilon_translation);
}

} // namespace ngine::math
