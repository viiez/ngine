module;

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

module ngine.script;

import ngine.core;
import ngine.input;
import ngine.math;
import ngine.physics;
import ngine.voxel;
import ngine.world;
import ngine.audio;
import :game_api;
import :world_api;

namespace ngine::script {

namespace {

struct Context {
    const input::ActionState* actions = nullptr;
    bool fixed = false;
    const physics::PhysicsWorld* physics = nullptr;
    audio::AudioSystem* audio = nullptr;
    audio::AudioEngine* audio_engine = nullptr;
    const world::TerrainMaterials* materials = nullptr;
    std::vector<events::Subscription> subscriptions;
    std::vector<NativeEvent> events;
};

Context& context() {
    static Context instance;
    return instance;
}

std::string_view text(const u8* data, i32 length) {
    return {reinterpret_cast<const char*>(data), static_cast<usize>(length < 0 ? 0 : length)};
}

math::DVec3 dvec3(const f64* v) {
    return {v[0], v[1], v[2]};
}
math::Vec3 vec3(const f32* v) {
    return {v[0], v[1], v[2]};
}
math::Quat quat(const f32* v) {
    return {v[0], v[1], v[2], v[3]};
}

physics::QueryFilter filter_of(const NativeFilter* filter) {
    if (filter == nullptr) {
        return {};
    }
    return {.layers = filter->layers, .ignore = filter->ignore, .sensors = filter->sensors != 0};
}

physics::QueryShape shape_of(const NativeShape& shape) {
    return {.shape = static_cast<physics::ShapeType>(shape.shape),
            .radius = shape.radius,
            .half_extents = vec3(shape.half_extents),
            .half_height = shape.half_height};
}

void write(NativeHit& out, u64 entity, math::DVec3 point, math::Vec3 normal, f32 distance,
           u32 material) {
    out = {.entity = entity,
           .point = {point.x, point.y, point.z},
           .normal = {normal.x, normal.y, normal.z},
           .distance = distance,
           .material = material};
}

i32 api_action(const u8* name, i32 length, NativeAction* out) {
    const Context& c = context();
    *out = {};
    if (c.actions == nullptr || !c.actions->map().find(text(name, length))) {
        return 0;
    }
    const input::ActionValue& value =
        c.fixed ? c.actions->fixed(text(name, length)) : c.actions->value(text(name, length));
    *out = {.x = value.x,
            .y = value.y,
            .down = static_cast<u8>(value.down),
            .pressed = static_cast<u8>(value.pressed),
            .released = static_cast<u8>(value.released)};
    return 1;
}

i32 api_raycast(const f64* origin, const f32* direction, f32 max_distance,
                const NativeFilter* filter, NativeHit* out) {
    const Context& c = context();
    if (c.physics == nullptr) {
        return 0;
    }
    const auto hit =
        c.physics->raycast(dvec3(origin), vec3(direction), max_distance, filter_of(filter));
    if (!hit) {
        return 0;
    }
    write(*out, hit->entity, hit->point, hit->normal, hit->distance, hit->material);
    return 1;
}

i32 api_shape_cast(const NativeShape* shape, const f64* origin, const f32* rotation,
                   const f32* direction, f32 max_distance, const NativeFilter* filter,
                   NativeHit* out) {
    const Context& c = context();
    if (c.physics == nullptr) {
        return 0;
    }
    const auto hit = c.physics->shape_cast(shape_of(*shape), dvec3(origin), quat(rotation),
                                           vec3(direction), max_distance, filter_of(filter));
    if (!hit) {
        return 0;
    }
    write(*out, hit->entity, hit->point, hit->normal, hit->distance, 0);
    return 1;
}

i32 api_overlap(const NativeShape* shape, const f64* position, const f32* rotation,
                const NativeFilter* filter, u64* out, i32 capacity) {
    const Context& c = context();
    if (c.physics == nullptr) {
        return 0;
    }
    const std::vector<u64> found =
        c.physics->overlap(shape_of(*shape), dvec3(position), quat(rotation), filter_of(filter));
    const usize count = std::min(found.size(), static_cast<usize>(std::max(capacity, 0)));
    if (out != nullptr && count > 0) {
        std::memcpy(out, found.data(), count * sizeof(u64));
    }
    return static_cast<i32>(found.size());
}

i32 api_terrain_edit(u64 terrain, const f64* point, f32 radius, i32 add, i32 material) {
    world::World* world = script_world();
    if (world == nullptr || terrain == 0 || world::Commands::is_pending(terrain) ||
        !world->alive(terrain)) {
        return -1;
    }
    world::Entity entity = world->entity(terrain);
    if (!entity.has<world::Terrain>()) {
        return -1;
    }
    const math::DVec3 center = world::terrain_voxels(entity, dvec3(point));
    const voxel::Brush brush{
        .mode = add != 0 ? voxel::BrushMode::Add : voxel::BrushMode::Remove,
        .center = {static_cast<f32>(center.x), static_cast<f32>(center.y),
                   static_cast<f32>(center.z)},
        .radius = static_cast<f32>(static_cast<f64>(radius) / voxel::voxel_size),
        .material = static_cast<voxel::Material>(std::clamp(material, 1, 255))};
    const voxel::TerrainEdit edit = world::edit_terrain(entity, brush);
    return edit.empty() ? 0 : 1;
}

audio::AudioBus bus_of(i32 bus) {
    return static_cast<audio::AudioBus>(std::clamp(bus, 0, static_cast<i32>(audio::bus_count) - 1));
}

i32 api_sound_play(u64 sound, const f64* position, i32 bus, f32 volume, f32 pitch) {
    audio::AudioSystem* system = context().audio;
    if (system == nullptr) {
        return 0;
    }
    std::optional<math::DVec3> at;
    if (position != nullptr) {
        at = dvec3(position);
    }
    return system->play_at(sound, at,
                           audio::OneShotParams{.bus = bus_of(bus), .volume = volume, .pitch = pitch})
               ? 1
               : 0;
}

i32 api_source_play(u64 entity, i32 play) {
    audio::AudioSystem* system = context().audio;
    world::World* world = script_world();
    if (system == nullptr || world == nullptr || !world->alive(entity)) {
        return 0;
    }
    if (play != 0) {
        system->play(world->entity(entity));
    } else {
        system->stop(world->entity(entity));
    }
    return 1;
}

i32 api_source_playing(u64 entity) {
    const audio::AudioSystem* system = context().audio;
    return system != nullptr && system->playing(entity) ? 1 : 0;
}

void api_set_bus_volume(i32 bus, f32 volume) {
    if (audio::AudioEngine* engine = context().audio_engine) {
        engine->set_bus_volume(bus_of(bus), volume);
    }
}

f32 api_bus_volume(i32 bus) {
    const audio::AudioEngine* engine = context().audio_engine;
    return engine != nullptr ? engine->bus_volume(bus_of(bus)) : 0.0f;
}

u64 api_footstep_sound(u32 material) {
    const world::TerrainMaterials* materials = context().materials;
    if (materials == nullptr) {
        return 0;
    }
    const world::TerrainMaterial* entry = materials->find(material);
    return entry != nullptr ? entry->footstep_sounds : 0;
}

} // namespace

GameApi make_game_api() {
    return GameApi{
        .action = api_action,
        .raycast = api_raycast,
        .shape_cast = api_shape_cast,
        .overlap = api_overlap,
        .terrain_edit = api_terrain_edit,
        .sound_play = api_sound_play,
        .source_play = api_source_play,
        .source_playing = api_source_playing,
        .set_bus_volume = api_set_bus_volume,
        .bus_volume = api_bus_volume,
        .footstep_sound = api_footstep_sound,
    };
}

void set_script_input_fixed(bool fixed) {
    context().fixed = fixed;
}

void bind_script_input(const input::ActionState* actions) {
    context().actions = actions;
}

void bind_script_physics(const physics::PhysicsWorld* physics) {
    context().physics = physics;
}

void bind_script_audio(audio::AudioSystem* audio, audio::AudioEngine* engine,
                       const world::TerrainMaterials* materials) {
    Context& c = context();
    c.audio = audio;
    c.audio_engine = engine;
    c.materials = materials;
}

void bind_script_events(events::EventBus* bus) {
    Context& c = context();
    c.subscriptions.clear();
    c.events.clear();
    if (bus == nullptr) {
        return;
    }
    auto& events = c.events;
    c.subscriptions.push_back(
        bus->subscribe<physics::ContactBegan>([&events](const physics::ContactBegan& e) {
            events.push_back({.kind = EventKind::ContactBegan,
                              .a = e.a,
                              .b = e.b,
                              .point = {e.point.x, e.point.y, e.point.z},
                              .normal = {e.normal.x, e.normal.y, e.normal.z}});
        }));
    c.subscriptions.push_back(
        bus->subscribe<physics::ContactEnded>([&events](const physics::ContactEnded& e) {
            events.push_back({.kind = EventKind::ContactEnded, .a = e.a, .b = e.b});
        }));
    c.subscriptions.push_back(
        bus->subscribe<physics::TriggerEntered>([&events](const physics::TriggerEntered& e) {
            events.push_back({.kind = EventKind::TriggerEntered, .a = e.sensor, .b = e.other});
        }));
    c.subscriptions.push_back(
        bus->subscribe<physics::TriggerLeft>([&events](const physics::TriggerLeft& e) {
            events.push_back({.kind = EventKind::TriggerLeft, .a = e.sensor, .b = e.other});
        }));
    c.subscriptions.push_back(
        bus->subscribe<physics::CharacterLanded>([&events](const physics::CharacterLanded& e) {
            events.push_back({.kind = EventKind::Landed,
                              .material = e.material,
                              .a = e.entity,
                              .value = e.height});
        }));
    c.subscriptions.push_back(
        bus->subscribe<physics::CharacterStep>([&events](const physics::CharacterStep& e) {
            events.push_back({.kind = EventKind::Step,
                              .material = e.material,
                              .a = e.entity,
                              .point = {e.position.x, e.position.y, e.position.z},
                              .value = e.run ? 1.0f : 0.0f});
        }));
}

std::vector<NativeEvent> take_script_events() {
    return std::exchange(context().events, {});
}

} // namespace ngine::script
