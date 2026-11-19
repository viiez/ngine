module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.animation;

import ngine.core;
import ngine.math;
import ngine.asset;
import ngine.profile;

namespace ngine::animation {

std::optional<u32> Rig::find_clip(std::string_view name) const noexcept {
    for (u32 i = 0; i < clips.size(); ++i) {
        if (clips[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<u32> Rig::find_node(std::string_view name) const noexcept {
    for (u32 i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

asset::Result<Rig> make_rig(const asset::Model& model) {
    Rig rig;
    const usize count = model.nodes.size();
    rig.parents.resize(count, -1);
    rig.names.resize(count);
    rig.rest.resize(count);
    for (usize i = 0; i < count; ++i) {
        const asset::Node& node = model.nodes[i];
        if (node.parent && *node.parent >= count) {
            return std::unexpected(std::format("Knoten {}: Elternknoten fehlt", i));
        }
        rig.parents[i] = node.parent ? static_cast<i32>(*node.parent) : -1;
        rig.names[i] = node.name;
        rig.rest[i] = {node.translation, node.rotation, node.scale};
    }
    // Eltern vor Kindern: Tiefensuche von den Wurzeln aus (Zyklen prüft schon der Import).
    std::vector<u32> stack;
    for (u32 i = 0; i < count; ++i) {
        if (rig.parents[i] < 0) {
            stack.push_back(i);
        }
    }
    while (!stack.empty()) {
        const u32 node = stack.back();
        stack.pop_back();
        rig.order.push_back(node);
        for (const u32 child : model.nodes[node].children) {
            stack.push_back(child);
        }
    }
    if (rig.order.size() != count) {
        return std::unexpected(std::string{"Hierarchie der Knoten ist nicht zusammenhängend"});
    }
    rig.skins = model.skins;
    rig.clips = model.animations;
    for (const asset::Animation& clip : rig.clips) {
        for (const asset::AnimationChannel& channel : clip.channels) {
            if (channel.node >= count) {
                return std::unexpected(std::format("Clip {}: Spur ohne Knoten", clip.name));
            }
        }
    }
    for (const asset::Skin& skin : rig.skins) {
        for (const u32 joint : skin.joints) {
            if (joint >= count) {
                return std::unexpected(std::format("Skin {}: Gelenk ohne Knoten", skin.name));
            }
        }
    }
    return rig;
}

void sample(const Rig& rig, u32 clip, f32 time, Pose& pose) {
    pose = rig.rest;
    if (clip >= rig.clips.size()) {
        return;
    }
    const asset::Animation& animation = rig.clips[clip];
    time = std::clamp(time, 0.0f, animation.duration);
    for (const asset::AnimationChannel& channel : animation.channels) {
        f32 value[4] = {};
        asset::sample_channel(channel, time, value);
        LocalTransform& local = pose[channel.node];
        switch (channel.path) {
        case asset::AnimationPath::Translation:
            local.translation = {value[0], value[1], value[2]};
            break;
        case asset::AnimationPath::Rotation:
            local.rotation = {value[0], value[1], value[2], value[3]};
            break;
        case asset::AnimationPath::Scale: local.scale = {value[0], value[1], value[2]}; break;
        }
    }
}

void blend(const Pose& a, const Pose& b, f32 weight, Pose& out) {
    const usize count = std::min(a.size(), b.size());
    out.resize(count);
    for (usize i = 0; i < count; ++i) {
        const LocalTransform& x = a[i];
        const LocalTransform& y = b[i];
        math::Quat rotation = y.rotation;
        if (math::dot(x.rotation, rotation) < 0.0f) {
            rotation = -rotation;
        }
        out[i] = {math::lerp(x.translation, y.translation, weight),
                  math::normalize(x.rotation + (rotation - x.rotation) * weight),
                  math::lerp(x.scale, y.scale, weight)};
    }
}

void model_space(const Rig& rig, const Pose& pose, std::vector<math::Mat4>& out) {
    out.resize(rig.node_count());
    for (const u32 node : rig.order) {
        const LocalTransform& local = pose[node];
        const math::Mat4 m = math::compose(local.translation, local.rotation, local.scale);
        const i32 parent = rig.parents[node];
        out[node] = parent >= 0 ? out[static_cast<usize>(parent)] * m : m;
    }
}

void palette(const Rig& rig, u32 skin, std::span<const math::Mat4> model,
             std::vector<JointMatrix>& out) {
    if (skin >= rig.skins.size()) {
        out.clear();
        return;
    }
    const asset::Skin& source = rig.skins[skin];
    out.resize(source.joints.size());
    for (usize j = 0; j < source.joints.size(); ++j) {
        const math::Mat4 m = model[source.joints[j]] * source.inverse_bind[j];
        for (usize r = 0; r < 3; ++r) {
            out[j].rows[r] = {m[0][r], m[1][r], m[2][r], m[3][r]};
        }
    }
}

void register_loaders(asset::ResourceManager& resources, const asset::AssetCache* cache) {
    resources.add_loader<Rig, Rig>(
        [cache](const asset::LoadContext& context) -> asset::Result<Rig> {
            NGINE_PROFILE_ZONE("animation::load_rig");
            const auto from_model = [&](const asset::Model& model) -> asset::Result<Rig> {
                auto rig = make_rig(model);
                if (!rig) {
                    return std::unexpected(std::format("{}: {}", context.path, rig.error()));
                }
                return std::move(*rig);
            };
            if (cache != nullptr) {
                auto cached = cache->load_model(context.id);
                if (cached && *cached) {
                    if (context.dependencies != nullptr) {
                        for (const asset::SourceStamp& stamp : (*cached)->sources) {
                            context.dependencies->push_back(stamp.path);
                        }
                    }
                    return from_model((*cached)->model);
                }
            }
            auto model = asset::load_gltf(context.path, context.dependencies);
            if (!model) {
                return std::unexpected(std::move(model.error()));
            }
            return from_model(*model);
        },
        [](Rig& rig) -> asset::Result<Rig> { return std::move(rig); });
}

} // namespace ngine::animation
