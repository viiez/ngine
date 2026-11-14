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
    if (types.find<CharacterController>() == nullptr) {
        types.add<CharacterController>("CharacterController")
            .field("radius", &CharacterController::radius)
            .range(0.05, 5.0, 0.01)
            .field("height", &CharacterController::height)
            .range(0.2, 10.0, 0.01)
            .field("crouch_height", &CharacterController::crouch_height)
            .range(0.2, 10.0, 0.01)
            .field("max_slope", &CharacterController::max_slope)
            .hint(reflect::Hint::Angle)
            .range(0.0, 1.5)
            .field("step_height", &CharacterController::step_height)
            .range(0.0, 2.0, 0.01)
            .field("jump_height", &CharacterController::jump_height)
            .range(0.0, 20.0, 0.01)
            .field("walk_speed", &CharacterController::walk_speed)
            .range(0.0, 100.0, 0.1)
            .field("run_speed", &CharacterController::run_speed)
            .range(0.0, 100.0, 0.1)
            .field("crouch_speed", &CharacterController::crouch_speed)
            .range(0.0, 100.0, 0.1)
            .field("ground_acceleration", &CharacterController::ground_acceleration)
            .range(0.0, 1000.0, 0.5)
            .field("air_acceleration", &CharacterController::air_acceleration)
            .range(0.0, 1000.0, 0.5)
            .field("gravity_factor", &CharacterController::gravity_factor)
            .field("mass", &CharacterController::mass)
            .range(0.0, 100000.0)
            .field("push_strength", &CharacterController::push_strength)
            .range(0.0, 100000.0)
            .field("step_length", &CharacterController::step_length)
            .range(0.05, 10.0, 0.01);
    }
    if (types.find<GroundState>() == nullptr) {
        types.add<GroundState>("GroundState")
            .value("Air", GroundState::Air)
            .value("Ground", GroundState::Ground)
            .value("Steep", GroundState::Steep);
    }
    if (types.find<CharacterMotion>() == nullptr) {
        types.add<CharacterMotion>("CharacterMotion")
            .transient()
            .field("move", &CharacterMotion::move)
            .field("run", &CharacterMotion::run)
            .field("crouch", &CharacterMotion::crouch)
            .field("jump", &CharacterMotion::jump)
            .field("velocity", &CharacterMotion::velocity)
            .field("ground", &CharacterMotion::ground)
            .field("ground_normal", &CharacterMotion::ground_normal)
            .field("ground_entity", &CharacterMotion::ground_entity)
            .field("ground_material", &CharacterMotion::ground_material)
            .field("crouched", &CharacterMotion::crouched);
    }
}

} // namespace ngine::physics
