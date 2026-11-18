module;

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

module ngine.asset;

import ngine.core;
import ngine.math;
import ngine.meshlet;

namespace ngine::asset {

namespace {

// Stelle des Schlüssels vor time und Anteil bis zum nächsten.
struct KeyPosition {
    usize key = 0;
    f32 t = 0.0f;
};

KeyPosition locate(std::span<const f32> times, f32 time) noexcept {
    if (times.size() < 2 || time <= times.front()) {
        return {};
    }
    if (time >= times.back()) {
        return {times.size() - 1, 0.0f};
    }
    const auto upper = std::upper_bound(times.begin(), times.end(), time);
    const usize key = static_cast<usize>(upper - times.begin()) - 1;
    const f32 span = times[key + 1] - times[key];
    return {key, span > 0.0f ? (time - times[key]) / span : 0.0f};
}

} // namespace

void sample_channel(const AnimationChannel& channel, f32 time, f32* out) noexcept {
    const usize width = channel.path == AnimationPath::Rotation ? 4 : 3;
    const KeyPosition at = locate(channel.times, time);
    const f32* a = &channel.values[at.key * width];
    if (at.t == 0.0f || channel.interpolation == Interpolation::Step ||
        at.key + 1 >= channel.times.size()) {
        std::copy_n(a, width, out);
        return;
    }
    const f32* b = a + width;
    if (width == 4) {
        const math::Quat qa{a[0], a[1], a[2], a[3]};
        math::Quat qb{b[0], b[1], b[2], b[3]};
        if (math::dot(qa, qb) < 0.0f) {
            qb = qb * -1.0f;
        }
        const math::Quat q = math::normalize(qa + (qb - qa) * at.t);
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
        return;
    }
    for (usize c = 0; c < width; ++c) {
        out[c] = a[c] + (b[c] - a[c]) * at.t;
    }
}

std::vector<math::Mat4> node_matrices(const Model& model, const Animation* animation, f32 time) {
    struct Local {
        math::Vec3 translation;
        math::Quat rotation;
        math::Vec3 scale;
    };
    std::vector<Local> locals(model.nodes.size());
    for (usize i = 0; i < model.nodes.size(); ++i) {
        const Node& node = model.nodes[i];
        locals[i] = {node.translation, node.rotation, node.scale};
    }
    if (animation != nullptr) {
        for (const AnimationChannel& channel : animation->channels) {
            if (channel.node >= locals.size()) {
                continue;
            }
            f32 value[4] = {};
            sample_channel(channel, time, value);
            Local& local = locals[channel.node];
            switch (channel.path) {
            case AnimationPath::Translation:
                local.translation = {value[0], value[1], value[2]};
                break;
            case AnimationPath::Rotation:
                local.rotation = {value[0], value[1], value[2], value[3]};
                break;
            case AnimationPath::Scale: local.scale = {value[0], value[1], value[2]}; break;
            }
        }
    }
    std::vector<math::Mat4> global(model.nodes.size());
    std::vector<bool> done(model.nodes.size(), false);
    // Eltern zuerst; die Kette ist beim Import auf Zyklen geprüft.
    std::vector<u32> chain;
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        chain.clear();
        for (std::optional<u32> n = i; n && !done[*n]; n = model.nodes[*n].parent) {
            chain.push_back(*n);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const Local& local = locals[*it];
            const math::Mat4 m = math::compose(local.translation, local.rotation, local.scale);
            const auto parent = model.nodes[*it].parent;
            global[*it] = parent ? global[*parent] * m : m;
            done[*it] = true;
        }
    }
    return global;
}

std::vector<math::Mat4> skin_palette(const Model& model, u32 skin,
                                     std::span<const math::Mat4> nodes) {
    const Skin& source = model.skins[skin];
    std::vector<math::Mat4> palette(source.joints.size());
    for (usize j = 0; j < source.joints.size(); ++j) {
        palette[j] = nodes[source.joints[j]] * source.inverse_bind[j];
    }
    return palette;
}

math::Vec3 skin_position(const SkinVertex& skin, math::Vec3 position,
                         std::span<const math::Mat4> palette) noexcept {
    const math::Vec4 p = math::extend(position, 1.0f);
    math::Vec4 result{0.0f, 0.0f, 0.0f, 0.0f};
    for (usize c = 0; c < 4; ++c) {
        if (skin.weights[c] == 0 || skin.joints[c] >= palette.size()) {
            continue;
        }
        result =
            result + palette[skin.joints[c]] * p * (static_cast<f32>(skin.weights[c]) / 65535.0f);
    }
    return {result.x, result.y, result.z};
}

void fit_skinned_meshlets(Model& model) {
    // Posen: Ruhelage und alle Clips mit 30 Hz, jeweils mit Anfang und Ende.
    std::vector<std::vector<math::Mat4>> poses;
    poses.push_back(node_matrices(model, nullptr, 0.0f));
    for (const Animation& animation : model.animations) {
        const usize steps = static_cast<usize>(animation.duration * 30.0f) + 1;
        for (usize step = 0; step <= steps; ++step) {
            const f32 time = std::min(animation.duration, static_cast<f32>(step) / 30.0f);
            poses.push_back(node_matrices(model, &animation, time));
        }
    }
    for (Mesh& mesh : model.meshes) {
        if (!mesh.skin || *mesh.skin >= model.skins.size()) {
            continue;
        }
        std::vector<std::vector<math::Mat4>> palettes;
        palettes.reserve(poses.size());
        for (const auto& pose : poses) {
            palettes.push_back(skin_palette(model, *mesh.skin, pose));
        }
        for (Primitive& primitive : mesh.primitives) {
            if (primitive.skin.size() != primitive.vertices.size()) {
                continue;
            }
            // Größter Abstand jedes Eckpunkts vom Mittelpunkt seiner Meshlets über alle Posen.
            std::vector<f32> radius(primitive.meshlets.size(), 0.0f);
            std::vector<math::Vec3> skinned(primitive.vertices.size());
            for (const auto& palette : palettes) {
                for (usize v = 0; v < skinned.size(); ++v) {
                    skinned[v] =
                        skin_position(primitive.skin[v], primitive.vertices[v].position, palette);
                }
                for (usize m = 0; m < primitive.meshlets.size(); ++m) {
                    const meshlet::Meshlet& meshlet = primitive.meshlets[m];
                    const usize first = usize{meshlet.first_triangle} * 3;
                    const usize last = first + usize{meshlet.triangle_count} * 3;
                    for (usize i = first; i < last && i < primitive.indices.size(); ++i) {
                        radius[m] = std::max(radius[m], math::length(skinned[primitive.indices[i]] -
                                                                     meshlet.center));
                    }
                }
            }
            for (usize m = 0; m < primitive.meshlets.size(); ++m) {
                meshlet::Meshlet& meshlet = primitive.meshlets[m];
                // Etwas Zuschlag für Zwischenwerte zwischen den abgetasteten Posen.
                meshlet.radius = std::max(meshlet.radius, radius[m] * 1.05f + 0.01f);
                meshlet.cone_cutoff = 1.0f;
            }
        }
    }
}

} // namespace ngine::asset
