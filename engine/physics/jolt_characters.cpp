module;

// Jolt nur in den Dateien jolt*.cpp und jolt.cppm (ADR 0045). Jolt.h zuerst: Es setzt die Schalter,
// die alle anderen Header brauchen.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Spielfiguren (R4.3, PHYS-03): CharacterVirtual von Jolt für Entities mit CharacterController.
module ngine.physics;

import ngine.core;
import ngine.math;
import ngine.jobs;
import ngine.profile;
import ngine.world;
import :jolt;

namespace ngine::physics {

JPH::BodyID PhysicsWorld::Impl::body_id_of(u64 entity) const {
    if (const auto found = bodies.find(entity); found != bodies.end()) {
        return found->second.id;
    }
    if (const auto found = characters.find(entity); found != characters.end()) {
        return found->second.character->GetInnerBodyID();
    }
    return {};
}

void PhysicsWorld::Impl::destroy_character(std::map<u64, CharacterRecord>::iterator it,
                                           std::vector<ContactEnded>& ended,
                                           std::vector<TriggerLeft>& left) {
    const u64 entity = it->first;
    entity_of_body.erase(it->second.character->GetInnerBodyID().GetIndexAndSequenceNumber());
    for (auto pair = touching.begin(); pair != touching.end();) {
        if (pair->first.first == entity || pair->first.second == entity) {
            emit_end(pair->first.first, pair->first.second, ended, left);
            pair = touching.erase(pair);
        } else {
            ++pair;
        }
    }
    // Der Destruktor nimmt den inneren Körper aus der Welt.
    characters.erase(it);
}

bool PhysicsWorld::Impl::create_character(u64 entity, const CharacterController& controller,
                                          const world::Transform& transform) {
    bool has_parent = false;
    const math::Decomposed pose = world_pose(entity, transform, has_parent);
    CharacterRecord record;
    record.standing = character_shape(controller.radius, controller.height);
    record.crouching =
        character_shape(controller.radius, std::min(controller.crouch_height, controller.height));
    if (record.standing == nullptr || record.crouching == nullptr) {
        warn_once(entity, "Spielfigur ohne gültige Form");
        return false;
    }
    JPH::Ref<JPH::CharacterVirtualSettings> character_settings =
        new JPH::CharacterVirtualSettings();
    character_settings->mID = JPH::CharacterID{static_cast<JPH::uint32>(entity)};
    character_settings->mShape = record.standing;
    character_settings->mMaxSlopeAngle = controller.max_slope;
    // Nur Berührungen unterhalb der unteren Halbkugel tragen die Figur.
    character_settings->mSupportingVolume =
        JPH::Plane{JPH::Vec3::sAxisY(), -std::max(controller.radius, 0.01f)};
    character_settings->mMass = controller.mass;
    character_settings->mMaxStrength = controller.push_strength;
    character_settings->mEnhancedInternalEdgeRemoval = true;
    character_settings->mInnerBodyShape = record.standing;
    character_settings->mInnerBodyLayer = object_layer(PhysicsLayer::Character);
    record.character = new JPH::CharacterVirtual(character_settings, to_jolt(pose.translation),
                                                 JPH::Quat::sIdentity(), entity, system.get());
    record.settings = controller;
    record.transform = transform;
    record.has_parent = has_parent;
    record.fall_top = pose.translation.y;
    entity_of_body[record.character->GetInnerBodyID().GetIndexAndSequenceNumber()] = entity;
    characters[entity] = std::move(record);
    return true;
}

// Legt Figuren an, entfernt und ändert sie (vor dem Schritt, nach den Körpern).
void PhysicsWorld::Impl::sync_characters(std::vector<ContactEnded>& ended,
                                         std::vector<TriggerLeft>& left) {
    std::vector<u64> current;
    world->each<const CharacterController, const world::Transform>(
        [&](world::Entity entity, const CharacterController&, const world::Transform&) {
            current.push_back(entity.id());
        });
    std::ranges::sort(current);
    for (auto it = characters.begin(); it != characters.end();) {
        if (!std::ranges::binary_search(current, it->first)) {
            auto next = std::next(it);
            destroy_character(it, ended, left);
            it = next;
        } else {
            ++it;
        }
    }
    // Strukturänderungen vor allen Zeigern in die Welt.
    for (const u64 entity : current) {
        if (!world->has(entity, motion_id)) {
            (void)world->ensure(entity, motion_id);
            world->modified(entity, motion_id);
        }
        if (!world->has(entity, interpolated_id)) {
            (void)world->ensure(entity, interpolated_id);
            world->modified(entity, interpolated_id);
        }
    }
    for (const u64 entity : current) {
        const auto* controller =
            static_cast<const CharacterController*>(world->get(entity, character_id));
        const auto* transform =
            static_cast<const world::Transform*>(world->get(entity, transform_id));
        auto found = characters.find(entity);
        if (found != characters.end()) {
            if (!same_character(found->second.settings, *controller)) {
                destroy_character(found, ended, left);
            } else {
                CharacterRecord& record = found->second;
                // Nur die Lage zählt: Die Steuerung dreht die Figur jedes Bild um die
                // Hochachse, das ist kein Versetzen.
                const world::Transform& before = record.transform;
                const bool moved = before.position.x != transform->position.x ||
                                   before.position.y != transform->position.y ||
                                   before.position.z != transform->position.z;
                record.transform = *transform;
                if (moved) {
                    // Von außen versetzt: Die Figur springt an die neue Lage.
                    bool has_parent = false;
                    const math::Decomposed pose = world_pose(entity, *transform, has_parent);
                    record.character->SetPosition(to_jolt(pose.translation));
                    record.character->SetLinearVelocity(JPH::Vec3::sZero());
                    record.has_parent = has_parent;
                    record.fall_top = pose.translation.y;
                }
                continue;
            }
        }
        (void)create_character(entity, *controller, *transform);
    }
}

// Bewegt alle Figuren um dt (vor dem Schritt der Körper, damit Kisten den Schub im selben
// Schritt spüren).
void PhysicsWorld::Impl::update_characters(f32 dt) {
    NGINE_PROFILE_ZONE("PhysicsWorld::characters");
    landed_events.clear();
    step_events.clear();
    const JPH::Vec3 gravity = system->GetGravity();
    const CharacterBodyFilter body_filter;
    const JPH::ShapeFilter shape_filter;
    const JPH::BroadPhaseLayerFilter& broad =
        system->GetDefaultBroadPhaseLayerFilter(object_layer(PhysicsLayer::Character));
    const JPH::ObjectLayerFilter& object =
        system->GetDefaultLayerFilter(object_layer(PhysicsLayer::Character));
    for (auto& [entity, record] : characters) {
        auto* motion = static_cast<CharacterMotion*>(world->get_mut(entity, motion_id));
        JPH::CharacterVirtual& character = *record.character;
        const CharacterController& c = record.settings;

        // Ohne Form des Terrains unter sich bleibt die Figur stehen.
        record.frozen = !terrains.empty() && missing_ground(from_jolt(character.GetPosition()));
        if (record.frozen) {
            character.SetLinearVelocity(JPH::Vec3::sZero());
            motion->velocity = {};
            motion->jump = false;
            continue;
        }

        // Ducken: Aufstehen nur, wenn über der Figur Platz ist.
        if (motion->crouch != record.crouched) {
            const JPH::Shape* target =
                motion->crouch ? record.crouching.GetPtr() : record.standing.GetPtr();
            if (character.SetShape(target, 1.5f * 0.02f, broad, object, body_filter, shape_filter,
                                   *temp)) {
                record.crouched = motion->crouch;
            }
        }

        const JPH::Vec3 g = gravity * c.gravity_factor;
        const JPH::Vec3 current = character.GetLinearVelocity();
        const JPH::CharacterBase::EGroundState state = character.GetGroundState();
        const bool on_ground = state == JPH::CharacterBase::EGroundState::OnGround;

        // Wunschgeschwindigkeit auf der Ebene.
        JPH::Vec3 wish{motion->move.x, 0.0f, motion->move.z};
        if (wish.LengthSq() > 1.0f) {
            wish = wish.Normalized();
        }
        const f32 speed = record.crouched ? c.crouch_speed
                          : motion->run   ? c.run_speed
                                          : c.walk_speed;
        wish *= speed;
        if (state == JPH::CharacterBase::EGroundState::OnSteepGround) {
            wish = character.CancelVelocityTowardsSteepSlopes(wish);
        }
        const JPH::Vec3 ground_velocity =
            on_ground ? character.GetGroundVelocity() : JPH::Vec3::sZero();
        JPH::Vec3 horizontal{current.GetX() - ground_velocity.GetX(), 0.0f,
                             current.GetZ() - ground_velocity.GetZ()};
        const f32 acceleration = on_ground ? c.ground_acceleration : c.air_acceleration;
        const JPH::Vec3 change = wish - horizontal;
        const f32 max_change = acceleration * dt;
        if (change.Length() <= max_change) {
            horizontal = wish;
        } else {
            horizontal += change.Normalized() * max_change;
        }

        f32 vertical = current.GetY();
        if (on_ground) {
            vertical = ground_velocity.GetY();
            if (motion->jump && c.jump_height > 0.0f) {
                vertical += std::sqrt(2.0f * g.Length() * c.jump_height);
            }
        }
        vertical += g.GetY() * dt;
        motion->jump = false;
        character.SetLinearVelocity(JPH::Vec3{horizontal.GetX() + ground_velocity.GetX(), vertical,
                                              horizontal.GetZ() + ground_velocity.GetZ()});

        const JPH::RVec3 before = character.GetPosition();
        const f32 before_vertical = character.GetLinearVelocity().GetY();
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mWalkStairsStepUp = JPH::Vec3{0.0f, c.step_height, 0.0f};
        update.mStickToFloorStepDown = JPH::Vec3{0.0f, -std::max(c.step_height, 0.1f), 0.0f};
        character.ExtendedUpdate(dt, g, update, broad, object, body_filter, shape_filter, *temp);

        const JPH::RVec3 after = character.GetPosition();
        const JPH::CharacterBase::EGroundState now = character.GetGroundState();
        const bool supported = now == JPH::CharacterBase::EGroundState::OnGround;
        motion->velocity = from_jolt(character.GetLinearVelocity());
        motion->ground = supported ? GroundState::Ground
                         : now == JPH::CharacterBase::EGroundState::OnSteepGround
                             ? GroundState::Steep
                             : GroundState::Air;
        motion->ground_normal = character.IsSupported() ? from_jolt(character.GetGroundNormal())
                                                        : math::Vec3{0.0f, 1.0f, 0.0f};
        motion->ground_entity =
            character.IsSupported() ? entity_of(character.GetGroundBodyID()) : 0;
        motion->ground_material = 0;
        if (character.IsSupported()) {
            if (const auto* surface =
                    dynamic_cast<const TerrainSurface*>(character.GetGroundMaterial())) {
                motion->ground_material = surface->material;
            }
        }
        motion->crouched = record.crouched;

        const f64 y = static_cast<f64>(after.GetY());
        if (!supported) {
            record.fall_top = record.supported ? y : std::max(record.fall_top, y);
            record.step_distance = 0.0;
        } else {
            if (!record.supported) {
                landed_events.push_back(
                    CharacterLanded{.entity = entity,
                                    .height = static_cast<f32>(std::max(record.fall_top - y, 0.0)),
                                    .speed = std::max(-before_vertical, 0.0f),
                                    .material = motion->ground_material});
            }
            const f64 dx = static_cast<f64>(after.GetX() - before.GetX());
            const f64 dz = static_cast<f64>(after.GetZ() - before.GetZ());
            record.step_distance += std::sqrt(dx * dx + dz * dz);
            if (record.step_distance >= static_cast<f64>(c.step_length)) {
                record.step_distance -= static_cast<f64>(c.step_length);
                step_events.push_back(CharacterStep{.entity = entity,
                                                    .position = from_jolt(after),
                                                    .material = motion->ground_material,
                                                    .run = motion->run});
            }
            record.fall_top = y;
        }
        record.supported = supported;

        // Fußpunkt zurück in Transform (lokal, wenn die Figur ein Elternteil hat).
        auto* transform = static_cast<world::Transform*>(world->get_mut(entity, transform_id));
        const math::DVec3 position = from_jolt(after);
        if (!record.has_parent) {
            transform->position = position;
        } else {
            const u64 parent = world->parent(entity);
            const math::Affine global =
                math::Affine::from(position, transform->rotation, {1.0f, 1.0f, 1.0f});
            transform->position =
                math::decompose(math::inverse(world::world_affine(*world, parent)) * global)
                    .translation;
        }
        world->modified(entity, transform_id);
        record.transform = *transform;
    }
}
} // namespace ngine::physics
