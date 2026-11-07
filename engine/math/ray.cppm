module;

#include <cmath>
#include <optional>

export module ngine.math:ray;

import ngine.core;
import :vector;

// Strahlen und Abstände für Treffertests, z. B. der Gizmos im Editor (R2.3). Gerechnet wird in
// 64 Bit, weil Strahl und Ziel weit vom Ursprung liegen können (ADR 0012).
export namespace ngine::math {

// Halbgerade ab origin in Richtung direction (normiert).
struct Ray {
    DVec3 origin;
    DVec3 direction{0.0, 0.0, -1.0};

    [[nodiscard]] constexpr DVec3 at(f64 t) const noexcept {
        return origin + direction * t;
    }
};

// Nächste Punkte zwischen einem Strahl und einer Geraden bzw. Strecke: ray_t auf dem Strahl
// (nie negativ), line_t auf der Geraden (in Längeneinheiten ab ihrem Startpunkt), distance ihr
// Abstand.
struct RayClosest {
    f64 ray_t = 0.0;
    f64 line_t = 0.0;
    f64 distance = 0.0;
};

// Gerade durch point in Richtung direction (normiert). Nichts, wenn beide fast parallel sind.
[[nodiscard]] inline std::optional<RayClosest> closest_to_line(const Ray& ray, DVec3 point,
                                                               DVec3 direction) noexcept {
    const DVec3 w = ray.origin - point;
    const f64 b = dot(ray.direction, direction);
    const f64 denominator = 1.0 - b * b;
    if (denominator < 1e-9) {
        return std::nullopt;
    }
    const f64 d = dot(ray.direction, w);
    const f64 e = dot(direction, w);
    f64 ray_t = (b * e - d) / denominator;
    f64 line_t = (e - b * d) / denominator;
    if (ray_t < 0.0) {
        // Der nächste Punkt läge hinter dem Strahl: vom Ursprung des Strahls aus messen.
        ray_t = 0.0;
        line_t = e;
    }
    return RayClosest{.ray_t = ray_t,
                      .line_t = line_t,
                      .distance = distance(ray.at(ray_t), point + direction * line_t)};
}

// Strecke von a nach b.
[[nodiscard]] inline RayClosest closest_to_segment(const Ray& ray, DVec3 a, DVec3 b) noexcept {
    const f64 extent = distance(a, b);
    const DVec3 direction = extent > 0.0 ? (b - a) / extent : DVec3{1.0, 0.0, 0.0};
    f64 line_t = 0.0;
    if (const auto closest = closest_to_line(ray, a, direction)) {
        line_t = closest->line_t;
    }
    line_t = line_t < 0.0 ? 0.0 : (line_t > extent ? extent : line_t);
    const DVec3 point = a + direction * line_t;
    f64 ray_t = dot(point - ray.origin, ray.direction);
    ray_t = ray_t < 0.0 ? 0.0 : ray_t;
    return RayClosest{.ray_t = ray_t, .line_t = line_t, .distance = distance(ray.at(ray_t), point)};
}

// Schnitt mit der Ebene durch point mit Normale normal: Parameter auf dem Strahl. Nichts, wenn
// der Strahl parallel zur Ebene läuft oder sie hinter sich hat.
[[nodiscard]] inline std::optional<f64> intersect_plane(const Ray& ray, DVec3 point,
                                                        DVec3 normal) noexcept {
    const f64 facing = dot(ray.direction, normal);
    if (std::abs(facing) < 1e-9) {
        return std::nullopt;
    }
    const f64 t = dot(point - ray.origin, normal) / facing;
    if (t < 0.0) {
        return std::nullopt;
    }
    return t;
}

// Erster Schnitt mit der Kugel (bzw. 0, wenn der Strahl in ihr beginnt).
[[nodiscard]] inline std::optional<f64> intersect_sphere(const Ray& ray, DVec3 center,
                                                         f64 radius) noexcept {
    const DVec3 offset = ray.origin - center;
    const f64 b = dot(offset, ray.direction);
    const f64 c = dot(offset, offset) - radius * radius;
    if (c <= 0.0) {
        return 0.0;
    }
    const f64 discriminant = b * b - c;
    if (b > 0.0 || discriminant < 0.0) {
        return std::nullopt;
    }
    return -b - std::sqrt(discriminant);
}

} // namespace ngine::math
