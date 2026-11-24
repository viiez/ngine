module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.animation;

import ngine.core;
import ngine.math;
import ngine.jobs;
import ngine.asset;
import ngine.world;
import ngine.profile;

namespace ngine::animation {

namespace {

// Vorfahren, die palette() höchstens hinaufsucht.
constexpr u32 max_ancestors = 16;

} // namespace

AnimationSystem::AnimationSystem(asset::ResourceManager& resources, jobs::JobSystem* jobs)
    : resources_{&resources}, jobs_{jobs} {}

AnimationSystem::~AnimationSystem() = default;

AnimationSystem::Instance* AnimationSystem::find(u64 entity) {
    const auto it = instances_.find(entity);
    return it != instances_.end() ? &it->second : nullptr;
}

const AnimationSystem::Instance* AnimationSystem::find(u64 entity) const {
    const auto it = instances_.find(entity);
    return it != instances_.end() ? &it->second : nullptr;
}

void AnimationSystem::prepare(Instance& instance) {
    if (instance.graph_id == 0) {
        instance.ready = false;
        instance.graph = {};
        instance.rig = {};
        return;
    }
    if (!instance.graph || instance.graph.id() != instance.graph_id) {
        instance.graph = resources_->load<AnimGraph>(instance.graph_id);
        instance.graph_version = 0;
        instance.ready = false;
    }
    const AnimGraph* graph = instance.graph.get();
    if (graph == nullptr) {
        if (instance.graph.state() == asset::ResourceState::Failed) {
            instance.error = instance.graph.error();
        }
        instance.ready = false;
        return;
    }
    if (!instance.rig || instance.rig.id() != graph->model) {
        instance.rig = resources_->load<Rig>(graph->model);
        instance.rig_version = 0;
        instance.ready = false;
    }
    const Rig* rig = instance.rig.get();
    if (rig == nullptr) {
        if (instance.rig.state() == asset::ResourceState::Failed) {
            instance.error = instance.rig.error();
        }
        instance.ready = false;
        return;
    }
    if (instance.ready && instance.graph_version == instance.graph.version() &&
        instance.rig_version == instance.rig.version()) {
        return;
    }
    // Neu anlegen (erstes Mal oder Hot Reload); Werte der Parameter bleiben, wo die Namen passen.
    std::vector<std::pair<std::string, f32>> values = std::move(instance.pending);
    instance.pending.clear();
    if (instance.ready) {
        for (usize i = 0; i < instance.machine.parameter_count(); ++i) {
            values.insert(values.begin(), {std::string{instance.machine.parameter_name(i)},
                                           instance.machine.parameter_value(i)});
        }
    }
    std::vector<std::string> warnings;
    auto machine = StateMachine::create(*graph, *rig, &warnings);
    // Warnungen einmal pro Stand von Automat und Rig, nicht für jede Figur, die ihn spielt.
    const std::pair<u64, u64> key{instance.graph_id,
                                  (static_cast<u64>(instance.graph.version()) << 32) |
                                      instance.rig.version()};
    const bool report = std::ranges::find(warned_, key) == warned_.end();
    if (report) {
        warned_.push_back(key);
    }
    for (const std::string& warning : warnings) {
        if (report) {
            log::warn("Animation (Entity {}): {}", instance.entity, warning);
        }
    }
    if (!machine) {
        instance.error = machine.error();
        if (report) {
            log::warn("Animation (Entity {}): {}", instance.entity, machine.error());
        }
        instance.ready = false;
        instance.graph_version = instance.graph.version();
        instance.rig_version = instance.rig.version();
        return;
    }
    instance.machine = std::move(*machine);
    for (const auto& [name, value] : values) {
        instance.machine.set_float(name, value);
    }
    instance.graph_version = instance.graph.version();
    instance.rig_version = instance.rig.version();
    instance.error.clear();
    instance.ready = true;
}

void AnimationSystem::run(Instance& instance, f32 dt) const {
    NGINE_PROFILE_ZONE("AnimationSystem::run");
    instance.fired.clear();
    instance.posed = false;
    if (!instance.ready || !instance.active) {
        return;
    }
    instance.machine.update(dt * instance.speed, &instance.fired);
    const bool wanted =
        instance.mode == PoseMode::Always || (instance.mode == PoseMode::Visible && rendering_);
    if (!wanted) {
        return;
    }
    const Rig& rig = *instance.rig.get();
    instance.machine.evaluate(rig, instance.pose);
    animation::model_space(rig, instance.pose, instance.model);
    instance.palettes.resize(rig.skins.size());
    for (u32 skin = 0; skin < rig.skins.size(); ++skin) {
        animation::palette(rig, skin, instance.model, instance.palettes[skin]);
    }
    instance.posed = true;
}

void AnimationSystem::update(world::World& world, f64 dt) {
    NGINE_PROFILE_ZONE("AnimationSystem::update");
    const auto begin = std::chrono::steady_clock::now();
    ++frame_;
    stats_ = {};
    world.query<const Animator>().each([&](world::Entity entity, const Animator& animator) {
        Instance& instance = instances_[entity.id()];
        instance.entity = entity.id();
        instance.graph_id = animator.graph;
        instance.speed = animator.speed;
        instance.active = animator.active;
        instance.mode = animator.pose_mode;
        instance.seen = frame_;
    });
    std::erase_if(instances_, [&](const auto& item) { return item.second.seen < frame_; });

    active_.clear();
    for (auto& [entity, instance] : instances_) {
        prepare(instance);
        ++stats_.animators;
        if (instance.ready) {
            ++stats_.running;
            active_.push_back(&instance);
        } else {
            instance.posed = false;
            instance.fired.clear();
            ++stats_.waiting;
        }
    }

    // Client: Parameter, wie sie der Server zuletzt geschickt hat (R5.2).
    constexpr usize synced_parameters = 4;
    if (sync_ == ParameterSync::Follow) {
        for (Instance* instance : active_) {
            const auto* synced = world.entity(instance->entity).get<AnimatorParameters>();
            if (synced == nullptr || std::ranges::find(local_, instance->entity) != local_.end()) {
                continue;
            }
            const std::array<f32, synced_parameters> values{synced->p0, synced->p1, synced->p2,
                                                            synced->p3};
            StateMachine& machine = instance->machine;
            const usize count = std::min(machine.parameter_count(), synced_parameters);
            for (usize i = 0; i < count; ++i) {
                if (machine.parameter_type(i) != ParameterType::Trigger) {
                    (void)machine.set_float(machine.parameter_name(i), values[i]);
                }
            }
        }
    }

    // Jeder Job schreibt nur in seine eigenen Instanzen.
    const f32 step = static_cast<f32>(dt);
    if (jobs_ != nullptr && active_.size() > 8) {
        jobs_->parallel_for(active_.size(), 8, [&](usize first, usize last) {
            for (usize i = first; i < last; ++i) {
                run(*active_[i], step);
            }
        });
    } else {
        for (Instance* instance : active_) {
            run(*instance, step);
        }
    }

    // Server: Parameter für die Replikation, nur wenn sie sich geändert haben.
    if (sync_ == ParameterSync::Publish) {
        for (const Instance* instance : active_) {
            const StateMachine& machine = instance->machine;
            std::array<f32, synced_parameters> values{};
            const usize count = std::min(machine.parameter_count(), synced_parameters);
            for (usize i = 0; i < count; ++i) {
                if (machine.parameter_type(i) != ParameterType::Trigger) {
                    values[i] = machine.parameter_value(i);
                }
            }
            const AnimatorParameters synced{
                .p0 = values[0], .p1 = values[1], .p2 = values[2], .p3 = values[3]};
            world::Entity entity = world.entity(instance->entity);
            const auto* current = entity.get<AnimatorParameters>();
            if (current == nullptr || *current != synced) {
                entity.set(synced);
            }
        }
    }

    // Ereignisse in fester Reihenfolge (nach Entity, dann nach Zeit im Zustand).
    for (const Instance* instance : active_) {
        if (instance->posed) {
            ++stats_.posed;
        }
        if (events_ == nullptr) {
            continue;
        }
        for (const std::string* name : instance->fired) {
            events_->publish(AnimationEvent{.entity = instance->entity, .name = *name});
        }
    }
    stats_.update_ms =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void AnimationSystem::set_local(u64 entity, bool local) {
    std::erase(local_, entity);
    if (local) {
        local_.push_back(entity);
    }
}

bool AnimationSystem::set_float(u64 entity, std::string_view name, f32 value) {
    Instance* instance = find(entity);
    if (instance == nullptr) {
        // Noch kein update() gesehen: merken, bis die Entity in der Abfrage auftaucht.
        instance = &instances_[entity];
        instance->entity = entity;
        instance->seen = frame_ + 1;
    }
    if (instance->ready) {
        return instance->machine.set_float(name, value);
    }
    std::erase_if(instance->pending, [&](const auto& item) { return item.first == name; });
    instance->pending.emplace_back(std::string{name}, value);
    return true;
}

bool AnimationSystem::set_bool(u64 entity, std::string_view name, bool value) {
    return set_float(entity, name, value ? 1.0f : 0.0f);
}

bool AnimationSystem::set_trigger(u64 entity, std::string_view name) {
    return set_float(entity, name, 1.0f);
}

std::optional<f32> AnimationSystem::parameter(u64 entity, std::string_view name) const {
    const Instance* instance = find(entity);
    if (instance == nullptr) {
        return std::nullopt;
    }
    if (instance->ready) {
        return instance->machine.parameter(name);
    }
    for (const auto& [key, value] : instance->pending) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

std::string_view AnimationSystem::state(u64 entity) const {
    const Instance* instance = find(entity);
    return instance != nullptr && instance->ready ? instance->machine.state() : std::string_view{};
}

std::span<const JointMatrix> AnimationSystem::palette(const world::World& world, u64 entity,
                                                      asset::AssetId model, u32 skin) const {
    u64 current = entity;
    for (u32 depth = 0; current != 0 && depth < max_ancestors; ++depth) {
        if (const Instance* instance = find(current)) {
            if (instance->posed && instance->rig.id() == model &&
                skin < instance->palettes.size()) {
                return instance->palettes[skin];
            }
        }
        current = world.parent(current);
    }
    return {};
}

std::span<const math::Mat4> AnimationSystem::model_space(u64 entity) const {
    const Instance* instance = find(entity);
    return instance != nullptr && instance->posed ? std::span<const math::Mat4>{instance->model}
                                                  : std::span<const math::Mat4>{};
}

std::optional<AnimatorView> AnimationSystem::view(u64 entity) const {
    const Instance* instance = find(entity);
    if (instance == nullptr) {
        return std::nullopt;
    }
    return AnimatorView{.machine = instance->ready ? &instance->machine : nullptr,
                        .rig = instance->rig.get(),
                        .model = instance->rig.id(),
                        .posed = instance->posed,
                        .error = instance->error};
}

void AnimationSystem::clear() {
    instances_.clear();
    active_.clear();
}

} // namespace ngine::animation
