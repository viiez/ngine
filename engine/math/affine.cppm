export module ngine.math:affine;

import ngine.core;
import :vector;
import :matrix;
import :quaternion;

export namespace ngine::math {

// Affine Abbildung mit gemischter Genauigkeit (ADR 0012): Drehung, Skalierung und Scherung in
// 32 Bit, Verschiebung in 64 Bit. Damit bleiben Weltpositionen weit vom Ursprung genau, ohne dass
// jede Matrix doppelt so groß wird. Gezeichnet wird relativ zur Kamera (relative_to()).
struct Affine {
    Mat3 linear;
    DVec3 translation;

    // Erst skalieren, dann drehen, dann verschieben.
    [[nodiscard]] static constexpr Affine from(DVec3 translation, Quat rotation,
                                               Vec3 scale) noexcept {
        const Mat3 r = to_mat3(rotation);
        return Affine{.linear = Mat3::from_columns(r[0] * scale.x, r[1] * scale.y, r[2] * scale.z),
                      .translation = translation};
    }

    // Die Verschiebung des Punktes entsteht in 64 Bit, auch der Anteil aus linear.
    [[nodiscard]] constexpr DVec3 transform_point(DVec3 point) const noexcept {
        return to_f64(linear) * point + translation;
    }
    [[nodiscard]] constexpr Vec3 transform_direction(Vec3 direction) const noexcept {
        return linear * direction;
    }

    // Matrix für die GPU, deren Ursprung in origin liegt (meist die Kameraposition). Die
    // Differenz entsteht in 64 Bit; erst das kleine Ergebnis wird auf 32 Bit gerundet.
    [[nodiscard]] constexpr Mat4 relative_to(DVec3 origin) const noexcept {
        Mat4 m = to_mat4(linear);
        m[3] = extend(to_f32(translation - origin), 1.0f);
        return m;
    }

    // Erst b, dann a anwenden (wie a * b bei Matrizen).
    friend constexpr Affine operator*(const Affine& a, const Affine& b) noexcept {
        return Affine{.linear = a.linear * b.linear,
                      .translation = a.transform_point(b.translation)};
    }
};

[[nodiscard]] constexpr Affine inverse(const Affine& a) noexcept {
    const Mat3 linear = inverse(a.linear);
    return Affine{.linear = linear, .translation = -(to_f64(linear) * a.translation)};
}

[[nodiscard]] inline bool near(const Affine& a, const Affine& b, f32 epsilon_linear,
                               f64 epsilon_translation) noexcept {
    return near(a.linear, b.linear, epsilon_linear) &&
           near(a.translation, b.translation, epsilon_translation);
}

} // namespace ngine::math
