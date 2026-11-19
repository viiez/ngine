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

// Abfragen (PHYS-04): Strahlen, Formen und Überlappung gegen Körper und Terrain.
module ngine.physics;

import ngine.core;
import ngine.math;
import ngine.jobs;
import ngine.profile;
import ngine.world;
import :jolt;

namespace ngine::physics {

JPH::Ref<JPH::Shape> PhysicsWorld::Impl::query_shape(const QueryShape& shape) const {
    return primitive_shape(shape.shape, shape.radius, shape.half_extents, shape.half_height,
                           {1.0f, 1.0f, 1.0f});
}

u64 PhysicsWorld::Impl::entity_of(JPH::BodyID id) const {
    const JPH::uint32 key = id.GetIndexAndSequenceNumber();
    if (const auto found = entity_of_body.find(key); found != entity_of_body.end()) {
        return found->second;
    }
    const auto terrain = terrain_of_body.find(key);
    return terrain != terrain_of_body.end() ? terrain->second : 0;
}

RayHit PhysicsWorld::Impl::ray_hit(const JPH::RRayCast& ray, const JPH::RayCastResult& result,
                                   f32 max_distance) const {
    RayHit hit;
    hit.entity = entity_of(result.mBodyID);
    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    hit.point = from_jolt(point);
    hit.distance = result.mFraction * max_distance;
    const JPH::BodyLockRead lock{system->GetBodyLockInterfaceNoLock(), result.mBodyID};
    if (lock.Succeeded()) {
        hit.normal =
            from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
        if (const auto material =
                terrain_material(*lock.GetBody().GetShape(), result.mSubShapeID2)) {
            hit.material = *material;
        }
    }
    return hit;
}

std::optional<RayHit> PhysicsWorld::raycast(math::DVec3 origin, math::Vec3 direction,
                                            f32 max_distance, QueryFilter filter) const {
    if (math::length_squared(direction) == 0.0f || max_distance <= 0.0f) {
        return std::nullopt;
    }
    const JPH::RRayCast ray{to_jolt(origin), to_jolt(math::normalize(direction) * max_distance)};
    JPH::RayCastResult result;
    const QueryBroadFilter broad{filter.layers};
    const QueryObjectFilter object{filter.layers};
    const QueryBodyFilter body{filter.ignore, filter.sensors};
    if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, result, broad, object, body)) {
        return std::nullopt;
    }
    return impl_->ray_hit(ray, result, max_distance);
}

std::vector<RayHit> PhysicsWorld::raycast_all(math::DVec3 origin, math::Vec3 direction,
                                              f32 max_distance, QueryFilter filter) const {
    std::vector<RayHit> hits;
    if (math::length_squared(direction) == 0.0f || max_distance <= 0.0f) {
        return hits;
    }
    const JPH::RRayCast ray{to_jolt(origin), to_jolt(math::normalize(direction) * max_distance)};
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const QueryBroadFilter broad{filter.layers};
    const QueryObjectFilter object{filter.layers};
    const QueryBodyFilter body{filter.ignore, filter.sensors};
    impl_->system->GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings{}, collector, broad,
                                                 object, body);
    collector.Sort();
    hits.reserve(collector.mHits.size());
    for (const JPH::RayCastResult& result : collector.mHits) {
        hits.push_back(impl_->ray_hit(ray, result, max_distance));
    }
    return hits;
}

std::optional<ShapeHit> PhysicsWorld::shape_cast(const QueryShape& shape, math::DVec3 origin,
                                                 math::Quat rotation, math::Vec3 direction,
                                                 f32 max_distance, QueryFilter filter) const {
    if (math::length_squared(direction) == 0.0f || max_distance <= 0.0f) {
        return std::nullopt;
    }
    const JPH::Ref<JPH::Shape> cast_shape = impl_->query_shape(shape);
    if (cast_shape == nullptr) {
        return std::nullopt;
    }
    const JPH::Vec3 movement = to_jolt(math::normalize(direction) * max_distance);
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        cast_shape.GetPtr(), JPH::Vec3::sOne(),
        JPH::RMat44::sRotationTranslation(to_jolt(math::normalize(rotation)), to_jolt(origin)),
        movement);
    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint = true;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const QueryBroadFilter broad{filter.layers};
    const QueryObjectFilter object{filter.layers};
    const QueryBodyFilter body{filter.ignore, filter.sensors};
    impl_->system->GetNarrowPhaseQuery().CastShape(cast, settings, to_jolt(origin), collector,
                                                   broad, object, body);
    if (!collector.HadHit()) {
        return std::nullopt;
    }
    const JPH::ShapeCastResult& result = collector.mHit;
    ShapeHit hit;
    hit.entity = impl_->entity_of(result.mBodyID2);
    hit.point = from_jolt(to_jolt(origin) + result.mContactPointOn2);
    const JPH::Vec3 axis = result.mPenetrationAxis;
    hit.normal =
        axis.LengthSq() > 0.0f ? from_jolt(-axis.Normalized()) : -math::normalize(direction);
    hit.distance = result.mFraction * max_distance;
    return hit;
}

std::vector<u64> PhysicsWorld::overlap(const QueryShape& shape, math::DVec3 position,
                                       math::Quat rotation, QueryFilter filter) const {
    std::vector<u64> result;
    const JPH::Ref<JPH::Shape> query = impl_->query_shape(shape);
    if (query == nullptr) {
        return result;
    }
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const QueryBroadFilter broad{filter.layers};
    const QueryObjectFilter object{filter.layers};
    const QueryBodyFilter body{filter.ignore, filter.sensors};
    const JPH::RMat44 transform =
        JPH::RMat44::sRotationTranslation(to_jolt(math::normalize(rotation)), to_jolt(position));
    impl_->system->GetNarrowPhaseQuery().CollideShape(
        query.GetPtr(), JPH::Vec3::sOne(), transform.PreTranslated(query->GetCenterOfMass()),
        JPH::CollideShapeSettings{}, to_jolt(position), collector, broad, object, body);
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        const u64 entity = impl_->entity_of(hit.mBodyID2);
        if (entity != 0) {
            result.push_back(entity);
        }
    }
    std::ranges::sort(result);
    const auto [first, last] = std::ranges::unique(result);
    result.erase(first, last);
    return result;
}

} // namespace ngine::physics
