module;

#include <array>

export module ngine.math:matrix;

import ngine.core;
import :vector;

export namespace ngine::math {

// Matrizen für Spaltenvektoren (p' = M * p), spaltenweise im Speicher wie in Vulkan und GLSL:
// m[spalte][zeile]. Mat3{} und Mat4{} sind die Einheitsmatrix.

template <typename T>
struct TMat3 {
    using Column = TVec3<T>;

    std::array<Column, 3> columns{Column{1, 0, 0}, Column{0, 1, 0}, Column{0, 0, 1}};

    [[nodiscard]] static constexpr TMat3 from_columns(Column x, Column y, Column z) noexcept {
        return TMat3{{x, y, z}};
    }

    [[nodiscard]] constexpr Column& operator[](usize column) noexcept {
        return columns[column];
    }
    [[nodiscard]] constexpr const Column& operator[](usize column) const noexcept {
        return columns[column];
    }

    friend constexpr Column operator*(const TMat3& m, Column v) noexcept {
        return m[0] * v.x + m[1] * v.y + m[2] * v.z;
    }
    friend constexpr TMat3 operator*(const TMat3& a, const TMat3& b) noexcept {
        return from_columns(a * b[0], a * b[1], a * b[2]);
    }
    friend constexpr TMat3 operator*(const TMat3& m, T s) noexcept {
        return from_columns(m[0] * s, m[1] * s, m[2] * s);
    }
    friend constexpr bool operator==(const TMat3&, const TMat3&) noexcept = default;
};

template <typename T>
struct TMat4 {
    using Column = TVec4<T>;

    std::array<Column, 4> columns{Column{1, 0, 0, 0}, Column{0, 1, 0, 0}, Column{0, 0, 1, 0},
                                  Column{0, 0, 0, 1}};

    [[nodiscard]] static constexpr TMat4 from_columns(Column x, Column y, Column z,
                                                      Column w) noexcept {
        return TMat4{{x, y, z, w}};
    }

    [[nodiscard]] constexpr Column& operator[](usize column) noexcept {
        return columns[column];
    }
    [[nodiscard]] constexpr const Column& operator[](usize column) const noexcept {
        return columns[column];
    }

    friend constexpr Column operator*(const TMat4& m, Column v) noexcept {
        return m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3] * v.w;
    }
    friend constexpr TMat4 operator*(const TMat4& a, const TMat4& b) noexcept {
        return from_columns(a * b[0], a * b[1], a * b[2], a * b[3]);
    }
    friend constexpr TMat4 operator*(const TMat4& m, T s) noexcept {
        return from_columns(m[0] * s, m[1] * s, m[2] * s, m[3] * s);
    }
    friend constexpr bool operator==(const TMat4&, const TMat4&) noexcept = default;
};

using Mat3 = TMat3<f32>;
using Mat4 = TMat4<f32>;
using DMat3 = TMat3<f64>;

[[nodiscard]] constexpr Mat3 mat3_identity() noexcept {
    return Mat3{};
}
[[nodiscard]] constexpr Mat4 mat4_identity() noexcept {
    return Mat4{};
}

// --- Umwandlungen -----------------------------------------------------------------------------

[[nodiscard]] constexpr DMat3 to_f64(const Mat3& m) noexcept {
    return DMat3::from_columns(to_f64(m[0]), to_f64(m[1]), to_f64(m[2]));
}
// Obere linke 3×3-Matrix.
[[nodiscard]] constexpr Mat3 to_mat3(const Mat4& m) noexcept {
    return Mat3::from_columns(xyz(m[0]), xyz(m[1]), xyz(m[2]));
}
// 3×3-Matrix als 4×4-Matrix ohne Verschiebung.
[[nodiscard]] constexpr Mat4 to_mat4(const Mat3& m) noexcept {
    return Mat4::from_columns(extend(m[0], 0.0f), extend(m[1], 0.0f), extend(m[2], 0.0f),
                              Vec4{0, 0, 0, 1});
}

// --- Funktionen -------------------------------------------------------------------------------

template <typename T>
[[nodiscard]] constexpr TMat3<T> transpose(const TMat3<T>& m) noexcept {
    return TMat3<T>::from_columns({m[0].x, m[1].x, m[2].x}, {m[0].y, m[1].y, m[2].y},
                                  {m[0].z, m[1].z, m[2].z});
}
template <typename T>
[[nodiscard]] constexpr TMat4<T> transpose(const TMat4<T>& m) noexcept {
    return TMat4<T>::from_columns(
        {m[0].x, m[1].x, m[2].x, m[3].x}, {m[0].y, m[1].y, m[2].y, m[3].y},
        {m[0].z, m[1].z, m[2].z, m[3].z}, {m[0].w, m[1].w, m[2].w, m[3].w});
}

template <typename T>
[[nodiscard]] constexpr T determinant(const TMat3<T>& m) noexcept {
    return dot(m[0], cross(m[1], m[2]));
}

// Umkehrmatrix über die Adjunkte. Bei einer singulären Matrix (Determinante 0) ist das Ergebnis
// unendlich oder NaN; wer das erwarten muss, prüft determinant() vorher.
template <typename T>
[[nodiscard]] constexpr TMat3<T> inverse(const TMat3<T>& m) noexcept {
    // Die Zeilen der Umkehrmatrix sind die Kreuzprodukte der Spalten, geteilt durch die
    // Determinante.
    const TVec3<T> r0 = cross(m[1], m[2]);
    const TVec3<T> r1 = cross(m[2], m[0]);
    const TVec3<T> r2 = cross(m[0], m[1]);
    const T inv_det = T{1} / dot(m[0], r0);
    return transpose(TMat3<T>::from_columns(r0, r1, r2)) * inv_det;
}

template <typename T>
[[nodiscard]] constexpr T determinant(const TMat4<T>& m) noexcept {
    // Entwicklung über 2×2-Unterdeterminanten der unteren beiden Zeilen.
    const T s0 = m[0].z * m[1].w - m[1].z * m[0].w;
    const T s1 = m[0].z * m[2].w - m[2].z * m[0].w;
    const T s2 = m[0].z * m[3].w - m[3].z * m[0].w;
    const T s3 = m[1].z * m[2].w - m[2].z * m[1].w;
    const T s4 = m[1].z * m[3].w - m[3].z * m[1].w;
    const T s5 = m[2].z * m[3].w - m[3].z * m[2].w;
    return m[0].x * (m[1].y * s5 - m[2].y * s4 + m[3].y * s3) -
           m[1].x * (m[0].y * s5 - m[2].y * s2 + m[3].y * s1) +
           m[2].x * (m[0].y * s4 - m[1].y * s2 + m[3].y * s0) -
           m[3].x * (m[0].y * s3 - m[1].y * s1 + m[2].y * s0);
}

// Allgemeine Umkehrmatrix über Kofaktoren (2×2-Unterdeterminanten der oberen und unteren
// Zeilenpaare). Für reine Drehungen und Verschiebungen ist transpose() bzw. Affine billiger.
template <typename T>
[[nodiscard]] constexpr TMat4<T> inverse(const TMat4<T>& m) noexcept {
    const T a00 = m[0].x, a01 = m[0].y, a02 = m[0].z, a03 = m[0].w;
    const T a10 = m[1].x, a11 = m[1].y, a12 = m[1].z, a13 = m[1].w;
    const T a20 = m[2].x, a21 = m[2].y, a22 = m[2].z, a23 = m[2].w;
    const T a30 = m[3].x, a31 = m[3].y, a32 = m[3].z, a33 = m[3].w;

    const T b00 = a00 * a11 - a01 * a10;
    const T b01 = a00 * a12 - a02 * a10;
    const T b02 = a00 * a13 - a03 * a10;
    const T b03 = a01 * a12 - a02 * a11;
    const T b04 = a01 * a13 - a03 * a11;
    const T b05 = a02 * a13 - a03 * a12;
    const T b06 = a20 * a31 - a21 * a30;
    const T b07 = a20 * a32 - a22 * a30;
    const T b08 = a20 * a33 - a23 * a30;
    const T b09 = a21 * a32 - a22 * a31;
    const T b10 = a21 * a33 - a23 * a31;
    const T b11 = a22 * a33 - a23 * a32;

    const T det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    const T inv = T{1} / det;

    return TMat4<T>::from_columns(
        {(a11 * b11 - a12 * b10 + a13 * b09) * inv, (a02 * b10 - a01 * b11 - a03 * b09) * inv,
         (a31 * b05 - a32 * b04 + a33 * b03) * inv, (a22 * b04 - a21 * b05 - a23 * b03) * inv},
        {(a12 * b08 - a10 * b11 - a13 * b07) * inv, (a00 * b11 - a02 * b08 + a03 * b07) * inv,
         (a32 * b02 - a30 * b05 - a33 * b01) * inv, (a20 * b05 - a22 * b02 + a23 * b01) * inv},
        {(a10 * b10 - a11 * b08 + a13 * b06) * inv, (a01 * b08 - a00 * b10 - a03 * b06) * inv,
         (a30 * b04 - a31 * b02 + a33 * b00) * inv, (a21 * b02 - a20 * b04 - a23 * b00) * inv},
        {(a11 * b07 - a10 * b09 - a12 * b06) * inv, (a00 * b09 - a01 * b07 + a02 * b06) * inv,
         (a31 * b01 - a30 * b03 - a32 * b00) * inv, (a20 * b03 - a21 * b01 + a22 * b00) * inv});
}

[[nodiscard]] inline bool near(const Mat3& a, const Mat3& b, f32 epsilon) noexcept {
    return near(a[0], b[0], epsilon) && near(a[1], b[1], epsilon) && near(a[2], b[2], epsilon);
}
[[nodiscard]] inline bool near(const Mat4& a, const Mat4& b, f32 epsilon) noexcept {
    return near(a[0], b[0], epsilon) && near(a[1], b[1], epsilon) && near(a[2], b[2], epsilon) &&
           near(a[3], b[3], epsilon);
}

} // namespace ngine::math
