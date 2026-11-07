module;

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

module ngine.edit;

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.world;

namespace ngine::edit {

namespace {

using math::DVec3;
using math::Vec3;

// Maße relativ zur Länge der Achsen.
constexpr f64 hit_radius = 0.07;
constexpr f64 axis_start = 0.2;
constexpr f64 axis_end = 1.1;
constexpr f64 center_radius = 0.16;
constexpr u32 ring_segments = 64;
constexpr f32 min_scale = 0.001f;

usize axis_index(GizmoAxis axis) noexcept {
    switch (axis) {
    case GizmoAxis::X: return 0;
    case GizmoAxis::Y: return 1;
    case GizmoAxis::Z: return 2;
    default: return 0;
    }
}

constexpr GizmoAxis axes[] = {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z};

DVec3 column(const math::Mat3& m, usize i) {
    return math::to_f64(m[i]);
}

f64 round_to(f64 value, f64 step) {
    return step > 0.0 ? std::round(value / step) * step : value;
}

char axis_name(GizmoAxis axis) {
    switch (axis) {
    case GizmoAxis::X: return 'x';
    case GizmoAxis::Y: return 'y';
    case GizmoAxis::Z: return 'z';
    default: return ' ';
    }
}

// Beschriftung des Schritts im Verlauf.
std::string_view mode_label(GizmoMode mode) {
    switch (mode) {
    case GizmoMode::Translate: return "Verschieben";
    case GizmoMode::Rotate: return "Drehen";
    case GizmoMode::Scale: return "Skalieren";
    }
    return "Transform";
}

const reflect::TypeInfo* transform_type(const SceneDocument& document) {
    return document.types().find<world::Transform>();
}

} // namespace

// --- ViewCamera ----------------------------------------------------------------------------------

math::Ray ViewCamera::ray(f32 x, f32 y) const noexcept {
    const f32 tan_half = std::tan(fov_y * 0.5f);
    const f32 aspect = height > 0.0f ? width / height : 1.0f;
    const f32 ndc_x = (x / width) * 2.0f - 1.0f;
    const f32 ndc_y = 1.0f - (y / height) * 2.0f;
    const Vec3 view{ndc_x * tan_half * aspect, ndc_y * tan_half, -1.0f};
    return math::Ray{.origin = position,
                     .direction = math::normalize(math::to_f64(rotation * view))};
}

std::optional<math::Vec2> ViewCamera::project(DVec3 point) const noexcept {
    // Kamerarelativ in 64 Bit, erst das kleine Ergebnis in 32 Bit (ADR 0012).
    const Vec3 view = math::transpose(rotation) * math::to_f32(point - position);
    const f32 depth = -view.z;
    if (depth <= near_plane) {
        return std::nullopt;
    }
    const f32 tan_half = std::tan(fov_y * 0.5f);
    const f32 aspect = height > 0.0f ? width / height : 1.0f;
    const f32 ndc_x = view.x / (depth * tan_half * aspect);
    const f32 ndc_y = view.y / (depth * tan_half);
    return math::Vec2{(ndc_x * 0.5f + 0.5f) * width, (0.5f - ndc_y * 0.5f) * height};
}

f64 ViewCamera::pixel_size(DVec3 point) const noexcept {
    const DVec3 forward = -math::to_f64(rotation[2]);
    const f64 depth = std::max(math::dot(point - position, forward), static_cast<f64>(near_plane));
    return 2.0 * depth * std::tan(static_cast<f64>(fov_y) * 0.5) / std::max(height, 1.0f);
}

// --- Gizmo ---------------------------------------------------------------------------------------

std::optional<GizmoFrame> Gizmo::frame(SceneDocument& document, EntityId entity,
                                       const ViewCamera& camera) const {
    const u64 handle = document.entity_of(entity);
    if (handle == 0 || !document.world().entity(handle).has<world::Transform>()) {
        return std::nullopt;
    }
    const math::Decomposed world = math::decompose(world::world_affine(document.world(), handle));
    GizmoFrame result{.origin = world.translation};
    if (space == GizmoSpace::Local || mode == GizmoMode::Scale) {
        result.axes = math::to_mat3(world.rotation);
    }
    result.size = camera.pixel_size(result.origin) * static_cast<f64>(size_pixels);
    return result;
}

GizmoAxis Gizmo::hit(const GizmoFrame& frame, const math::Ray& ray,
                     const ViewCamera& camera) const {
    const f64 radius = frame.size * hit_radius;
    GizmoAxis best = GizmoAxis::None;
    f64 best_score = 0.0;
    const auto consider = [&](GizmoAxis axis, f64 score) {
        if (best == GizmoAxis::None || score < best_score) {
            best = axis;
            best_score = score;
        }
    };

    if (mode == GizmoMode::Scale) {
        if (math::intersect_sphere(ray, frame.origin, frame.size * center_radius)) {
            return GizmoAxis::All;
        }
    }
    const DVec3 view = math::normalize(frame.origin - camera.position);
    for (const GizmoAxis axis : axes) {
        const DVec3 direction = column(frame.axes, axis_index(axis));
        if (mode == GizmoMode::Rotate) {
            // Ring um die Achse: Schnitt mit seiner Ebene, Abstand vom Kreis.
            if (std::abs(math::dot(ray.direction, direction)) < 0.02) {
                continue;
            }
            const auto t = math::intersect_plane(ray, frame.origin, direction);
            if (!t) {
                continue;
            }
            const f64 off = std::abs(math::distance(ray.at(*t), frame.origin) - frame.size);
            if (off < radius) {
                // Der vorderste Ring gewinnt.
                consider(axis, *t);
            }
        } else {
            // Achsen, die fast auf die Kamera zeigen, sind kaum zu sehen und nicht zu treffen.
            if (std::abs(math::dot(view, direction)) > 0.98) {
                continue;
            }
            const math::RayClosest closest =
                math::closest_to_segment(ray, frame.origin + direction * (frame.size * axis_start),
                                         frame.origin + direction * (frame.size * axis_end));
            if (closest.distance < radius) {
                consider(axis, closest.distance);
            }
        }
    }
    return best;
}

GizmoShape Gizmo::shape(const GizmoFrame& frame) const {
    GizmoShape shape;
    for (const GizmoAxis axis : axes) {
        const usize i = axis_index(axis);
        const DVec3 direction = column(frame.axes, i);
        if (mode == GizmoMode::Rotate) {
            const DVec3 u = column(frame.axes, (i + 1) % 3);
            const DVec3 v = column(frame.axes, (i + 2) % 3);
            DVec3 previous = frame.origin + u * frame.size;
            for (u32 s = 1; s <= ring_segments; ++s) {
                const f64 angle = 2.0 * math::pi_f64 * static_cast<f64>(s) / ring_segments;
                const DVec3 next =
                    frame.origin + (u * std::cos(angle) + v * std::sin(angle)) * frame.size;
                shape.lines.push_back({.a = previous, .b = next, .axis = axis});
                previous = next;
            }
            continue;
        }
        const DVec3 tip = frame.origin + direction * frame.size;
        shape.lines.push_back(
            {.a = frame.origin + direction * (frame.size * axis_start), .b = tip, .axis = axis});
        shape.markers.push_back({.position = tip,
                                 .direction = direction,
                                 .handle = mode == GizmoMode::Translate ? GizmoShape::Handle::Arrow
                                                                        : GizmoShape::Handle::Box,
                                 .axis = axis});
    }
    if (mode == GizmoMode::Scale) {
        shape.markers.push_back({.position = frame.origin,
                                 .direction = {},
                                 .handle = GizmoShape::Handle::Box,
                                 .axis = GizmoAxis::All});
    }
    return shape;
}

void Gizmo::reset() noexcept {
    drag_.reset();
    hovered_ = GizmoAxis::None;
}

std::optional<world::Transform> Gizmo::drag_to(Drag& drag, const math::Ray& ray) const {
    world::Transform result = drag.start;
    const GizmoFrame& frame = drag.frame;
    switch (mode) {
    case GizmoMode::Translate: {
        const auto closest = math::closest_to_line(ray, frame.origin, drag.direction);
        if (!closest) {
            return std::nullopt;
        }
        f64 delta = closest->line_t - drag.start_t;
        if (snap.enabled) {
            if (space == GizmoSpace::World) {
                // Auf das Raster der Welt: die Koordinate entlang der Achse rastet ein.
                const usize i = axis_index(drag.axis);
                delta = round_to(frame.origin[i] + delta, snap.translate) - frame.origin[i];
            } else {
                delta = round_to(delta, snap.translate);
            }
        }
        // Verschiebung in der Welt, umgerechnet in die Achsen des Elternteils.
        const DVec3 moved = drag.direction * delta;
        result.position =
            drag.start.position + math::to_f64(math::inverse(drag.parent.linear)) * moved;
        drag.value = delta;
        break;
    }
    case GizmoMode::Rotate: {
        if (std::abs(math::dot(ray.direction, drag.direction)) < 0.02) {
            return std::nullopt;
        }
        const auto t = math::intersect_plane(ray, frame.origin, drag.direction);
        if (!t) {
            return std::nullopt;
        }
        const DVec3 vector = math::normalize(ray.at(*t) - frame.origin);
        if (math::length_squared(vector) == 0.0) {
            return std::nullopt;
        }
        // Winkel in kleinen Schritten aufsummieren, damit mehr als eine halbe Drehung geht.
        drag.angle += std::atan2(math::dot(math::cross(drag.last_vector, vector), drag.direction),
                                 math::dot(drag.last_vector, vector));
        drag.last_vector = vector;
        const f64 angle =
            snap.enabled ? round_to(drag.angle, static_cast<f64>(snap.rotate)) : drag.angle;
        const math::Quat turn =
            math::angle_axis(static_cast<f32>(angle), math::to_f32(drag.direction));
        // Drehung in der Welt, in die Achsen des Elternteils übersetzt.
        const math::Quat parent = math::decompose(drag.parent).rotation;
        result.rotation =
            math::normalize(math::inverse(parent) * turn * parent * drag.start.rotation);
        drag.value = static_cast<f64>(math::degrees(static_cast<f32>(angle)));
        break;
    }
    case GizmoMode::Scale: {
        const auto closest = math::closest_to_line(ray, frame.origin, drag.direction);
        if (!closest) {
            return std::nullopt;
        }
        // Nach innen höchstens bis 0; min_scale verhindert eine Skalierung von 0.
        const f32 factor =
            std::max(static_cast<f32>(1.0 + (closest->line_t - drag.start_t) / frame.size), 0.0f);
        const auto apply = [&](f32 start) {
            f32 value = start * factor;
            if (snap.enabled) {
                value = static_cast<f32>(round_to(value, static_cast<f64>(snap.scale)));
            }
            if (std::abs(value) < min_scale) {
                value = start < 0.0f ? -min_scale : min_scale;
            }
            return value;
        };
        if (drag.axis == GizmoAxis::All) {
            result.scale = {apply(drag.start.scale.x), apply(drag.start.scale.y),
                            apply(drag.start.scale.z)};
        } else {
            const usize i = axis_index(drag.axis);
            result.scale[i] = apply(drag.start.scale[i]);
        }
        drag.value = factor;
        break;
    }
    }
    return result;
}

bool Gizmo::update(SceneDocument& document, EntityId entity, const ViewCamera& camera,
                   const GizmoInput& input) {
    const math::Ray ray = camera.ray(input.x, input.y);
    if (drag_) {
        if (!input.down || drag_->entity != entity || !document.exists(entity)) {
            // Losgelassen (oder die Entity ist weg): Das Ziehen ist ein Schritt im Verlauf.
            drag_.reset();
            document.history().seal();
            return true;
        }
        const auto transform = drag_to(*drag_, ray);
        if (transform) {
            const reflect::TypeInfo* type = transform_type(document);
            auto command = set_field(document, entity, FieldPath{.component = type->id},
                                     &*transform, std::string{mode_label(mode)});
            if (!command) {
                log::error("{}", command.error());
            } else if (*command) {
                if (auto result = document.execute(*std::move(command), true); !result) {
                    log::error("{}", result.error());
                }
            }
        }
        return true;
    }

    hovered_ = GizmoAxis::None;
    if (!input.hovered || document.read_only()) {
        return false;
    }
    const auto current = frame(document, entity, camera);
    if (!current) {
        return false;
    }
    hovered_ = hit(*current, ray, camera);
    if (hovered_ == GizmoAxis::None || !input.pressed) {
        return false;
    }

    // Ziehen beginnen: Ausgangslage merken, damit jedes Frame vom Start aus rechnet.
    const u64 handle = document.entity_of(entity);
    world::World& world = document.world();
    Drag drag{.entity = entity, .axis = hovered_, .frame = *current};
    drag.start = *world.entity(handle).get<world::Transform>();
    if (const u64 parent = world.parent(handle); parent != 0) {
        drag.parent = world::world_affine(world, parent);
    }
    if (hovered_ == GizmoAxis::All) {
        // Gleichmäßig: entlang der Bildschirmwaagerechten.
        drag.direction = math::to_f64(camera.rotation[0]);
    } else {
        drag.direction = column(current->axes, axis_index(hovered_));
    }
    if (mode == GizmoMode::Rotate) {
        const auto t = math::intersect_plane(ray, current->origin, drag.direction);
        drag.last_vector =
            t ? math::normalize(ray.at(*t) - current->origin) : column(current->axes, 0);
    } else if (const auto closest = math::closest_to_line(ray, current->origin, drag.direction)) {
        drag.start_t = closest->line_t;
    }
    drag.value = mode == GizmoMode::Scale ? 1.0 : 0.0;
    drag_ = std::move(drag);
    // Ein Ziehen beginnt einen neuen Schritt, auch direkt nach einer Änderung im Inspector.
    document.history().seal();
    return true;
}

std::string Gizmo::drag_label() const {
    if (!drag_) {
        return {};
    }
    const char axis = drag_->axis == GizmoAxis::All ? ' ' : axis_name(drag_->axis);
    switch (mode) {
    case GizmoMode::Translate: return std::format("Verschieben {} {:+.2f} m", axis, drag_->value);
    case GizmoMode::Rotate: return std::format("Drehen {} {:+.1f}°", axis, drag_->value);
    case GizmoMode::Scale:
        return drag_->axis == GizmoAxis::All
                   ? std::format("Skalieren ×{:.2f}", drag_->value)
                   : std::format("Skalieren {} ×{:.2f}", axis, drag_->value);
    }
    return {};
}

} // namespace ngine::edit
