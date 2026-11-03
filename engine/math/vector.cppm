module;

#include <cmath>

export module ngine.math:vector;

import ngine.core;
import :scalar;

export namespace ngine::math {

// Vektoren sind Aggregate mit den Feldern x, y, z, w: Vec3{1, 2, 3} setzt alle Komponenten,
// Vec3{} ist der Nullvektor. Die Reflection beschreibt sie wie jede andere Struktur. Gerechnet
// wird skalar; der Compiler vektorisiert, wo es lohnt (ADR 0025).

template <typename T>
struct TVec2 {
    T x{};
    T y{};

    [[nodiscard]] constexpr T& operator[](usize i) noexcept {
        return i == 0 ? x : y;
    }
    [[nodiscard]] constexpr const T& operator[](usize i) const noexcept {
        return i == 0 ? x : y;
    }

    friend constexpr TVec2 operator+(TVec2 a, TVec2 b) noexcept {
        return {a.x + b.x, a.y + b.y};
    }
    friend constexpr TVec2 operator-(TVec2 a, TVec2 b) noexcept {
        return {a.x - b.x, a.y - b.y};
    }
    friend constexpr TVec2 operator*(TVec2 a, TVec2 b) noexcept {
        return {a.x * b.x, a.y * b.y};
    }
    friend constexpr TVec2 operator/(TVec2 a, TVec2 b) noexcept {
        return {a.x / b.x, a.y / b.y};
    }
    friend constexpr TVec2 operator*(TVec2 a, T s) noexcept {
        return {a.x * s, a.y * s};
    }
    friend constexpr TVec2 operator*(T s, TVec2 a) noexcept {
        return {s * a.x, s * a.y};
    }
    friend constexpr TVec2 operator/(TVec2 a, T s) noexcept {
        return {a.x / s, a.y / s};
    }
    friend constexpr TVec2 operator-(TVec2 a) noexcept {
        return {-a.x, -a.y};
    }
    constexpr TVec2& operator+=(TVec2 b) noexcept {
        return *this = *this + b;
    }
    constexpr TVec2& operator-=(TVec2 b) noexcept {
        return *this = *this - b;
    }
    constexpr TVec2& operator*=(T s) noexcept {
        return *this = *this * s;
    }
    constexpr TVec2& operator/=(T s) noexcept {
        return *this = *this / s;
    }
    friend constexpr bool operator==(TVec2, TVec2) noexcept = default;
};

template <typename T>
struct TVec3 {
    T x{};
    T y{};
    T z{};

    [[nodiscard]] constexpr T& operator[](usize i) noexcept {
        return i == 0 ? x : (i == 1 ? y : z);
    }
    [[nodiscard]] constexpr const T& operator[](usize i) const noexcept {
        return i == 0 ? x : (i == 1 ? y : z);
    }

    friend constexpr TVec3 operator+(TVec3 a, TVec3 b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    friend constexpr TVec3 operator-(TVec3 a, TVec3 b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    friend constexpr TVec3 operator*(TVec3 a, TVec3 b) noexcept {
        return {a.x * b.x, a.y * b.y, a.z * b.z};
    }
    friend constexpr TVec3 operator/(TVec3 a, TVec3 b) noexcept {
        return {a.x / b.x, a.y / b.y, a.z / b.z};
    }
    friend constexpr TVec3 operator*(TVec3 a, T s) noexcept {
        return {a.x * s, a.y * s, a.z * s};
    }
    friend constexpr TVec3 operator*(T s, TVec3 a) noexcept {
        return {s * a.x, s * a.y, s * a.z};
    }
    friend constexpr TVec3 operator/(TVec3 a, T s) noexcept {
        return {a.x / s, a.y / s, a.z / s};
    }
    friend constexpr TVec3 operator-(TVec3 a) noexcept {
        return {-a.x, -a.y, -a.z};
    }
    constexpr TVec3& operator+=(TVec3 b) noexcept {
        return *this = *this + b;
    }
    constexpr TVec3& operator-=(TVec3 b) noexcept {
        return *this = *this - b;
    }
    constexpr TVec3& operator*=(T s) noexcept {
        return *this = *this * s;
    }
    constexpr TVec3& operator/=(T s) noexcept {
        return *this = *this / s;
    }
    friend constexpr bool operator==(TVec3, TVec3) noexcept = default;
};

template <typename T>
struct TVec4 {
    T x{};
    T y{};
    T z{};
    T w{};

    [[nodiscard]] constexpr T& operator[](usize i) noexcept {
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }
    [[nodiscard]] constexpr const T& operator[](usize i) const noexcept {
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }

    friend constexpr TVec4 operator+(TVec4 a, TVec4 b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    friend constexpr TVec4 operator-(TVec4 a, TVec4 b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    friend constexpr TVec4 operator*(TVec4 a, TVec4 b) noexcept {
        return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
    }
    friend constexpr TVec4 operator/(TVec4 a, TVec4 b) noexcept {
        return {a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w};
    }
    friend constexpr TVec4 operator*(TVec4 a, T s) noexcept {
        return {a.x * s, a.y * s, a.z * s, a.w * s};
    }
    friend constexpr TVec4 operator*(T s, TVec4 a) noexcept {
        return {s * a.x, s * a.y, s * a.z, s * a.w};
    }
    friend constexpr TVec4 operator/(TVec4 a, T s) noexcept {
        return {a.x / s, a.y / s, a.z / s, a.w / s};
    }
    friend constexpr TVec4 operator-(TVec4 a) noexcept {
        return {-a.x, -a.y, -a.z, -a.w};
    }
    constexpr TVec4& operator+=(TVec4 b) noexcept {
        return *this = *this + b;
    }
    constexpr TVec4& operator-=(TVec4 b) noexcept {
        return *this = *this - b;
    }
    constexpr TVec4& operator*=(T s) noexcept {
        return *this = *this * s;
    }
    constexpr TVec4& operator/=(T s) noexcept {
        return *this = *this / s;
    }
    friend constexpr bool operator==(TVec4, TVec4) noexcept = default;
};

using Vec2 = TVec2<f32>;
using Vec3 = TVec3<f32>;
using Vec4 = TVec4<f32>;
using IVec2 = TVec2<i32>;
using IVec3 = TVec3<i32>;
// Weltpositionen in 64 Bit (ADR 0012): auf 10 000 km vom Ursprung noch genauer als ein Nanometer.
using DVec3 = TVec3<f64>;

// --- Umwandlungen -----------------------------------------------------------------------------

[[nodiscard]] constexpr Vec3 to_f32(DVec3 v) noexcept {
    return {static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
}
[[nodiscard]] constexpr DVec3 to_f64(Vec3 v) noexcept {
    return {v.x, v.y, v.z};
}
template <typename T>
[[nodiscard]] constexpr TVec3<T> xyz(TVec4<T> v) noexcept {
    return {v.x, v.y, v.z};
}
template <typename T>
[[nodiscard]] constexpr TVec4<T> extend(TVec3<T> v, T w) noexcept {
    return {v.x, v.y, v.z, w};
}

// --- Funktionen -------------------------------------------------------------------------------

template <typename T>
[[nodiscard]] constexpr T dot(TVec2<T> a, TVec2<T> b) noexcept {
    return a.x * b.x + a.y * b.y;
}
template <typename T>
[[nodiscard]] constexpr T dot(TVec3<T> a, TVec3<T> b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
template <typename T>
[[nodiscard]] constexpr T dot(TVec4<T> a, TVec4<T> b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

template <typename T>
[[nodiscard]] constexpr TVec3<T> cross(TVec3<T> a, TVec3<T> b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

template <typename V>
[[nodiscard]] constexpr auto length_squared(V v) noexcept {
    return dot(v, v);
}
template <typename V>
[[nodiscard]] inline auto length(V v) noexcept {
    return std::sqrt(dot(v, v));
}
template <typename V>
[[nodiscard]] inline auto distance(V a, V b) noexcept {
    return length(a - b);
}

// Einheitsvektor in Richtung v. Der Nullvektor bleibt der Nullvektor (GLM liefert dort NaN).
template <typename V>
[[nodiscard]] inline V normalize(V v) noexcept {
    const auto len = length(v);
    using T = decltype(len);
    return len > T{0} ? v * (T{1} / len) : V{};
}

template <typename V, typename T>
[[nodiscard]] constexpr V lerp(V a, V b, T t) noexcept {
    return a + (b - a) * t;
}

template <typename T>
[[nodiscard]] constexpr TVec3<T> min(TVec3<T> a, TVec3<T> b) noexcept {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
template <typename T>
[[nodiscard]] constexpr TVec3<T> max(TVec3<T> a, TVec3<T> b) noexcept {
    return {a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y, a.z < b.z ? b.z : a.z};
}
template <typename T>
[[nodiscard]] constexpr TVec3<T> clamp(TVec3<T> v, TVec3<T> low, TVec3<T> high) noexcept {
    return min(max(v, low), high);
}
template <typename T>
[[nodiscard]] inline TVec3<T> abs(TVec3<T> v) noexcept {
    return {std::abs(v.x), std::abs(v.y), std::abs(v.z)};
}

[[nodiscard]] inline bool near(Vec2 a, Vec2 b, f32 epsilon) noexcept {
    return near(a.x, b.x, epsilon) && near(a.y, b.y, epsilon);
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

} // namespace ngine::math
