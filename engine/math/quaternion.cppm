module;

#include <cmath>

export module ngine.math:quaternion;

import ngine.core;
import :scalar;
import :vector;
import :matrix;

export namespace ngine::math {

// Einheitsquaternion für Drehungen. Felder in der Reihenfolge x, y, z, w (Vektorteil, dann
// Skalarteil); Quat{} ist die Drehung um 0.
struct Quat {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f;

    // Hamilton-Produkt: erst b, dann a drehen.
    friend constexpr Quat operator*(Quat a, Quat b) noexcept {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    // Dreht v. Kurzform von q * (v, 0) * q⁻¹ für Einheitsquaternionen.
    friend constexpr Vec3 operator*(Quat q, Vec3 v) noexcept {
        const Vec3 u{q.x, q.y, q.z};
        const Vec3 t = cross(u, v) * 2.0f;
        return v + t * q.w + cross(u, t);
    }
    friend constexpr Quat operator*(Quat q, f32 s) noexcept {
        return {q.x * s, q.y * s, q.z * s, q.w * s};
    }
    friend constexpr Quat operator+(Quat a, Quat b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    friend constexpr Quat operator-(Quat a, Quat b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    friend constexpr Quat operator-(Quat q) noexcept {
        return {-q.x, -q.y, -q.z, -q.w};
    }
    friend constexpr bool operator==(Quat, Quat) noexcept = default;
};

[[nodiscard]] constexpr Quat quat_identity() noexcept {
    return Quat{};
}

[[nodiscard]] constexpr f32 dot(Quat a, Quat b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
[[nodiscard]] constexpr Quat conjugate(Quat q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}
[[nodiscard]] constexpr Quat inverse(Quat q) noexcept {
    return conjugate(q) * (1.0f / dot(q, q));
}

// Drehung um axis (normiert) um angle im Bogenmaß, gegen den Uhrzeigersinn, wenn axis auf den
// Betrachter zeigt.
[[nodiscard]] inline Quat angle_axis(f32 angle, Vec3 axis) noexcept {
    const f32 s = std::sin(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
}

// Kugelförmige Interpolation auf dem kürzeren Weg; bei fast gleichen Drehungen linear.
[[nodiscard]] inline Quat slerp(Quat a, Quat b, f32 t) noexcept {
    f32 cos_theta = dot(a, b);
    if (cos_theta < 0.0f) {
        b = -b;
        cos_theta = -cos_theta;
    }
    if (cos_theta > 1.0f - 1e-6f) {
        return normalize(a + (b - a) * t);
    }
    const f32 theta = std::acos(cos_theta);
    const f32 sin_theta = std::sin(theta);
    return a * (std::sin((1.0f - t) * theta) / sin_theta) + b * (std::sin(t * theta) / sin_theta);
}

[[nodiscard]] constexpr Mat3 to_mat3(Quat q) noexcept {
    const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return Mat3::from_columns({1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy)},
                              {2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)},
                              {2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy)});
}
[[nodiscard]] constexpr Mat4 to_mat4(Quat q) noexcept {
    return to_mat4(to_mat3(q));
}

// Drehung aus einer reinen Drehmatrix (orthonormal, Determinante 1), nach Shepperd: wählt die
// größte Diagonalsumme, damit nie durch eine kleine Zahl geteilt wird.
[[nodiscard]] inline Quat to_quat(const Mat3& m) noexcept {
    const f32 trace = m[0].x + m[1].y + m[2].z;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        return {(m[1].z - m[2].y) / s, (m[2].x - m[0].z) / s, (m[0].y - m[1].x) / s, 0.25f * s};
    }
    if (m[0].x > m[1].y && m[0].x > m[2].z) {
        const f32 s = std::sqrt(1.0f + m[0].x - m[1].y - m[2].z) * 2.0f;
        return {0.25f * s, (m[1].x + m[0].y) / s, (m[2].x + m[0].z) / s, (m[1].z - m[2].y) / s};
    }
    if (m[1].y > m[2].z) {
        const f32 s = std::sqrt(1.0f + m[1].y - m[0].x - m[2].z) * 2.0f;
        return {(m[1].x + m[0].y) / s, 0.25f * s, (m[2].y + m[1].z) / s, (m[2].x - m[0].z) / s};
    }
    const f32 s = std::sqrt(1.0f + m[2].z - m[0].x - m[1].y) * 2.0f;
    return {(m[2].x + m[0].z) / s, (m[2].y + m[1].z) / s, 0.25f * s, (m[0].y - m[1].x) / s};
}

// Drehung, die -z auf forward und y möglichst auf up abbildet (Blickrichtung einer Kamera).
// forward darf nicht parallel zu up sein.
[[nodiscard]] inline Quat look_rotation(Vec3 forward, Vec3 up) noexcept {
    const Vec3 back = -normalize(forward);
    const Vec3 right = normalize(cross(up, back));
    const Vec3 true_up = cross(back, right);
    return to_quat(Mat3::from_columns(right, true_up, back));
}

// Gleiche Drehung: q und -q drehen gleich.
[[nodiscard]] inline bool near(Quat a, Quat b, f32 epsilon) noexcept {
    return std::abs(std::abs(dot(a, b)) - 1.0f) <= epsilon;
}

} // namespace ngine::math
