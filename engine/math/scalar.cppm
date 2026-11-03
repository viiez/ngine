module;

#include <cmath>

export module ngine.math:scalar;

import ngine.core;

export namespace ngine::math {

inline constexpr f32 pi = 3.14159265358979323846f;
inline constexpr f64 pi_f64 = 3.14159265358979323846;

[[nodiscard]] constexpr f32 radians(f32 degrees) noexcept {
    return degrees * (pi / 180.0f);
}
[[nodiscard]] constexpr f32 degrees(f32 radians) noexcept {
    return radians * (180.0f / pi);
}

template <typename T>
[[nodiscard]] constexpr T clamp(T value, T low, T high) noexcept {
    return value < low ? low : (high < value ? high : value);
}

// Vergleich mit absoluter Toleranz (kleiner oder gleich), für Tests und Prüfungen.
[[nodiscard]] inline bool near(f32 a, f32 b, f32 epsilon) noexcept {
    return std::abs(a - b) <= epsilon;
}
[[nodiscard]] inline bool near(f64 a, f64 b, f64 epsilon) noexcept {
    return std::abs(a - b) <= epsilon;
}

} // namespace ngine::math
