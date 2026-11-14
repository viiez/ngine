module ngine.physics;

import ngine.core;
import ngine.reflect;

namespace ngine::physics {

void register_types(reflect::TypeRegistry& types) {
    if (types.find<MotionType>() == nullptr) {
        types.add<MotionType>("MotionType")
            .value("Static", MotionType::Static)
            .value("Kinematic", MotionType::Kinematic)
            .value("Dynamic", MotionType::Dynamic);
    }
    if (types.find<PhysicsLayer>() == nullptr) {
        types.add<PhysicsLayer>("PhysicsLayer")
            .value("Static", PhysicsLayer::Static)
            .value("Moving", PhysicsLayer::Moving)
            .value("Character", PhysicsLayer::Character)
            .value("Debris", PhysicsLayer::Debris)
            .value("Sensor", PhysicsLayer::Sensor);
    }
    if (types.find<ShapeType>() == nullptr) {
        types.add<ShapeType>("ShapeType")
            .value("Sphere", ShapeType::Sphere)
            .value("Box", ShapeType::Box)
            .value("Capsule", ShapeType::Capsule)
            .value("ConvexHull", ShapeType::ConvexHull)
            .value("Mesh", ShapeType::Mesh);
    }
    if (types.find<RigidBody>() == nullptr) {
        types.add<RigidBody>("RigidBody")
            .field("motion", &RigidBody::motion)
            .field("layer", &RigidBody::layer)
            .field("mass", &RigidBody::mass)
            .range(0.0, 100000.0)
            .field("friction", &RigidBody::friction)
            .range(0.0, 2.0, 0.01)
            .field("restitution", &RigidBody::restitution)
            .range(0.0, 1.0, 0.01)
            .field("linear_damping", &RigidBody::linear_damping)
            .range(0.0, 10.0, 0.01)
            .field("angular_damping", &RigidBody::angular_damping)
            .range(0.0, 10.0, 0.01)
            .field("gravity_factor", &RigidBody::gravity_factor)
            .field("allow_sleep", &RigidBody::allow_sleep)
            .field("linear_velocity", &RigidBody::linear_velocity)
            .field("angular_velocity", &RigidBody::angular_velocity);
    }
    if (types.find<Collider>() == nullptr) {
        types.add<Collider>("Collider")
            .field("shape", &Collider::shape)
            .field("radius", &Collider::radius)
            .range(0.001, 1000.0, 0.01)
            .field("half_extents", &Collider::half_extents)
            .field("half_height", &Collider::half_height)
            .range(0.0, 1000.0, 0.01)
            .field("offset", &Collider::offset)
            .field("model", &Collider::model)
            .hint(reflect::Hint::Asset)
            .field("mesh", &Collider::mesh)
            .field("sensor", &Collider::sensor);
    }
}

} // namespace ngine::physics
