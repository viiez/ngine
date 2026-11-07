module ngine.world;

import ngine.core;
import ngine.math;

namespace ngine::world {

usize update_transforms(World& world) {
    auto visit = [](void* /*context*/, u32 count, const void* own, void* out, const void* parent) {
        const auto* locals = static_cast<const Transform*>(own);
        auto* globals = static_cast<WorldTransform*>(out);
        const auto* parent_global = static_cast<const WorldTransform*>(parent);
        if (parent_global == nullptr) {
            for (u32 i = 0; i < count; ++i) {
                globals[i].value = locals[i].local();
            }
        } else {
            for (u32 i = 0; i < count; ++i) {
                globals[i].value = parent_global->value * locals[i].local();
            }
        }
    };
    return world.for_each_parent_first_changed(
        world.component<Transform>(), world.component<WorldTransform>(),
        HierarchyVisitor{.context = nullptr, .visit = visit});
}

math::Affine world_affine(World& world, u64 entity) {
    math::Affine result;
    const ComponentId transform = world.component<Transform>();
    for (u64 current = entity; current != 0; current = world.parent(current)) {
        if (const auto* local = static_cast<const Transform*>(world.get(current, transform))) {
            result = local->local() * result;
        }
    }
    return result;
}

} // namespace ngine::world
